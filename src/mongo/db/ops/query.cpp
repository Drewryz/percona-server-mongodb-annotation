// query.cpp

/**
 *    Copyright (C) 2008 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pch.h"
#include "query.h"
#include "../pdfile.h"
#include "../clientcursor.h"
#include "../oplog.h"
#include "../../bson/util/builder.h"
#include "../replutil.h"
#include "../scanandorder.h"
#include "../commands.h"
#include "../queryoptimizer.h"
#include "../../s/d_logic.h"
#include "../../server.h"
#include "../queryoptimizercursor.h"

namespace mongo {

    /* We cut off further objects once we cross this threshold; thus, you might get
       a little bit more than this, it is a threshold rather than a limit.
    */
    const int MaxBytesToReturnToClientAtOnce = 4 * 1024 * 1024;

    bool runCommands(const char *ns, BSONObj& jsobj, CurOp& curop, BufBuilder &b, BSONObjBuilder& anObjBuilder, bool fromRepl, int queryOptions) {
        try {
            return _runCommands(ns, jsobj, b, anObjBuilder, fromRepl, queryOptions);
        }
        catch( SendStaleConfigException& ){
            throw;
        }
        catch ( AssertionException& e ) {
            assert( e.getCode() != SendStaleConfigCode && e.getCode() != RecvStaleConfigCode );

            e.getInfo().append( anObjBuilder , "assertion" , "assertionCode" );
            curop.debug().exceptionInfo = e.getInfo();
        }
        anObjBuilder.append("errmsg", "db assertion failure");
        anObjBuilder.append("ok", 0.0);
        BSONObj x = anObjBuilder.done();
        b.appendBuf((void*) x.objdata(), x.objsize());
        return true;
    }


    BSONObj id_obj = fromjson("{\"_id\":1}");
    BSONObj empty_obj = fromjson("{}");


    //int dump = 0;

    /* empty result for error conditions */
    QueryResult* emptyMoreResult(long long cursorid) {
        BufBuilder b(32768);
        b.skip(sizeof(QueryResult));
        QueryResult *qr = (QueryResult *) b.buf();
        qr->cursorId = 0; // 0 indicates no more data to retrieve.
        qr->startingFrom = 0;
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->initializeResultFlags();
        qr->nReturned = 0;
        b.decouple();
        return qr;
    }

    QueryResult* processGetMore(const char *ns, int ntoreturn, long long cursorid , CurOp& curop, int pass, bool& exhaust ) {
        exhaust = false;
        ClientCursor::Pointer p(cursorid);
        ClientCursor *cc = p.c();

        int bufSize = 512 + sizeof( QueryResult ) + MaxBytesToReturnToClientAtOnce;

        BufBuilder b( bufSize );
        b.skip(sizeof(QueryResult));
        int resultFlags = ResultFlag_AwaitCapable;
        int start = 0;
        int n = 0;

        if ( unlikely(!cc) ) {
            LOGSOME << "getMore: cursorid not found " << ns << " " << cursorid << endl;
            cursorid = 0;
            resultFlags = ResultFlag_CursorNotFound;
        }
        else {
            // check for spoofing of the ns such that it does not match the one originally there for the cursor
            uassert(14833, "auth error", str::equals(ns, cc->ns().c_str()));

            if ( pass == 0 )
                cc->updateSlaveLocation( curop );

            int queryOptions = cc->queryOptions();
            
            curop.debug().query = cc->query();

            start = cc->pos();
            Cursor *c = cc->c();
            c->recoverFromYield();
            DiskLoc last;

            scoped_ptr<Projection::KeyOnly> keyFieldsOnly;
            if ( cc->modifiedKeys() == false && cc->isMultiKey() == false && cc->fields )
                keyFieldsOnly.reset( cc->fields->checkKey( cc->indexKeyPattern() ) );

            // This manager may be stale, but it's the state of chunking when the cursor was created.
            ShardChunkManagerPtr manager = cc->getChunkManager();

            while ( 1 ) {
                if ( !c->ok() ) {
                    if ( c->tailable() ) {
                        /* when a tailable cursor hits "EOF", ok() goes false, and current() is null.  however
                           advance() can still be retries as a reactivation attempt.  when there is new data, it will
                           return true.  that's what we are doing here.
                           */
                        if ( c->advance() )
                            continue;

                        if( n == 0 && (queryOptions & QueryOption_AwaitData) && pass < 1000 ) {
                            return 0;
                        }

                        break;
                    }
                    p.release();
                    bool ok = ClientCursor::erase(cursorid);
                    assert(ok);
                    cursorid = 0;
                    cc = 0;
                    break;
                }

                // in some cases (clone collection) there won't be a matcher
                if ( c->matcher() && !c->matcher()->matchesCurrent( c ) ) {
                }
                else if ( manager && ! manager->belongsToMe( cc ) ){
                    LOG(2) << "cursor skipping document in un-owned chunk: " << c->current() << endl;
                }
                else {
                    if( c->getsetdup(c->currLoc()) ) {
                        //out() << "  but it's a dup \n";
                    }
                    else {
                        last = c->currLoc();
                        n++;

                        if ( keyFieldsOnly ) {
                            fillQueryResultFromObj(b, 0, keyFieldsOnly->hydrate( c->currKey() ) );
                        }
                        else {
                            BSONObj js = c->current();
                            // show disk loc should be part of the main query, not in an $or clause, so this should be ok
                            fillQueryResultFromObj(b, cc->fields.get(), js, ( cc->pq.get() && cc->pq->showDiskLoc() ? &last : 0));
                        }

                        if ( ( ntoreturn && n >= ntoreturn ) || b.len() > MaxBytesToReturnToClientAtOnce ) {
                            c->advance();
                            cc->incPos( n );
                            break;
                        }
                    }
                }
                c->advance();

                if ( ! cc->yieldSometimes( ClientCursor::MaybeCovered ) ) {
                    ClientCursor::erase(cursorid);
                    cursorid = 0;
                    cc = 0;
                    p.deleted();
                    break;
                }
            }
            
            if ( cc ) {
                if ( c->supportYields() ) {
                    ClientCursor::YieldData data;
                    assert( cc->prepareToYield( data ) );
                }
                else {
                    cc->updateLocation();
                }
                cc->mayUpgradeStorage();
                cc->storeOpForSlave( last );
                exhaust = cc->queryOptions() & QueryOption_Exhaust;
            }
        }

        QueryResult *qr = (QueryResult *) b.buf();
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->_resultFlags() = resultFlags;
        qr->cursorId = cursorid;
        qr->startingFrom = start;
        qr->nReturned = n;
        b.decouple();

        return qr;
    }

    ExplainRecordingStrategy::ExplainRecordingStrategy
    ( const ExplainQueryInfo::AncillaryInfo &ancillaryInfo ) :
    _ancillaryInfo( ancillaryInfo ) {
    }

    shared_ptr<ExplainQueryInfo> ExplainRecordingStrategy::doneQueryInfo() {
        shared_ptr<ExplainQueryInfo> ret = _doneQueryInfo();
        ret->setAncillaryInfo( _ancillaryInfo );
        return ret;
    }
    
    NoExplainStrategy::NoExplainStrategy() :
    ExplainRecordingStrategy( ExplainQueryInfo::AncillaryInfo() ) {
    }

    shared_ptr<ExplainQueryInfo> NoExplainStrategy::_doneQueryInfo() {
        verify( 16071, false );
        return shared_ptr<ExplainQueryInfo>();
    }
    
    SimpleCursorExplainStrategy::SimpleCursorExplainStrategy
    ( const ExplainQueryInfo::AncillaryInfo &ancillaryInfo,
     const shared_ptr<Cursor> &cursor ) :
    ExplainRecordingStrategy( ancillaryInfo ),
    _cursor( cursor ),
    _explainInfo( new ExplainSinglePlanQueryInfo() ) {
    }
 
    void SimpleCursorExplainStrategy::notePlan( bool scanAndOrder, bool indexOnly ) {
        _explainInfo->notePlan( *_cursor, scanAndOrder, indexOnly );
    }

    void SimpleCursorExplainStrategy::noteIterate( bool match, bool loadedObject, bool chunkSkip ) {
        _explainInfo->noteIterate( match, loadedObject, chunkSkip, *_cursor );
    }

    void SimpleCursorExplainStrategy::noteYield() {
        _explainInfo->noteYield();
    }

    shared_ptr<ExplainQueryInfo> SimpleCursorExplainStrategy::_doneQueryInfo() {
        _explainInfo->noteDone( *_cursor );
        return _explainInfo->queryInfo();
    }

    QueryOptimizerCursorExplainStrategy::QueryOptimizerCursorExplainStrategy
    ( const ExplainQueryInfo::AncillaryInfo &ancillaryInfo,
     const shared_ptr<QueryOptimizerCursor> &cursor ) :
    ExplainRecordingStrategy( ancillaryInfo ),
    _cursor( cursor ) {
    }
    
    void QueryOptimizerCursorExplainStrategy::noteIterate( bool match, bool loadedObject,
                                                          bool chunkSkip ) {
        _cursor->noteIterate( match, loadedObject, chunkSkip );
    }

    shared_ptr<ExplainQueryInfo> QueryOptimizerCursorExplainStrategy::_doneQueryInfo() {
        return _cursor->explainQueryInfo();
    }

    ResponseBuildStrategy::ResponseBuildStrategy( const ParsedQuery &parsedQuery,
                                                 const shared_ptr<Cursor> &cursor, BufBuilder &buf,
                                                 const QueryPlan::Summary &queryPlan ) :
    _parsedQuery( parsedQuery ),
    _cursor( cursor ),
    _queryOptimizerCursor( dynamic_pointer_cast<QueryOptimizerCursor>( _cursor ) ),
    _buf( buf ),
    _planKeyFieldsOnly( queryPlan._keyFieldsOnly ) {
    }

    void ResponseBuildStrategy::resetBuf() {
        _buf.reset();
        _buf.skip( sizeof( QueryResult ) );
    }

    BSONObj ResponseBuildStrategy::current( bool allowCovered ) const {
        if ( _parsedQuery.returnKey() ) {
            BSONObjBuilder bob;
            bob.appendKeys( _cursor->indexKeyPattern(), _cursor->currKey() );
            return bob.obj();
        }
        if ( allowCovered ) {
            const Projection::KeyOnly *fields = keyFieldsOnly();
            if ( fields ) {
                return fields->hydrate( _cursor->currKey() );
            }
        }
        BSONObj ret = _cursor->current();
        verify( 16087, ret.isValid() );
        return ret;
    }

    const Projection::KeyOnly *ResponseBuildStrategy::keyFieldsOnly() const {
        if ( !_parsedQuery.getFields() ) {
            return 0;
        }
        if ( _queryOptimizerCursor ) {
            return _queryOptimizerCursor->keyFieldsOnly();
        }
        return _planKeyFieldsOnly.get();
    }
    
    OrderedBuildStrategy::OrderedBuildStrategy( const ParsedQuery &parsedQuery,
                                               const shared_ptr<Cursor> &cursor,
                                               BufBuilder &buf,
                                               const QueryPlan::Summary &queryPlan ) :
    ResponseBuildStrategy( parsedQuery, cursor, buf, queryPlan ),
    _skip( _parsedQuery.getSkip() ) {
    }
    
    bool OrderedBuildStrategy::handleMatch() {
        DiskLoc loc = _cursor->currLoc();
        if ( _cursor->getsetdup( loc ) ) {
            return false;
        }
        if ( _skip > 0 ) {
            --_skip;
            return false;
        }
        // Explain does not obey soft limits, so matches should not be buffered.
        if ( !_parsedQuery.isExplain() ) {
            fillQueryResultFromObj( _buf, _parsedQuery.getFields(), current( true ),
                                   ( _parsedQuery.showDiskLoc() ? &loc : 0 ) );
        }
        return true;
    }
    
    ReorderBuildStrategy::ReorderBuildStrategy( const ParsedQuery &parsedQuery,
                                               const shared_ptr<Cursor> &cursor,
                                               BufBuilder &buf,
                                               const QueryPlan::Summary &queryPlan ) :
    ResponseBuildStrategy( parsedQuery, cursor, buf, queryPlan ),
    _scanAndOrder( newScanAndOrder( queryPlan ) ) {
    }

    bool ReorderBuildStrategy::handleMatch() {
        if ( _cursor->getsetdup( _cursor->currLoc() ) ) {
            return false;
        }
        return _handleMatchNoDedup();
    }
    
    bool ReorderBuildStrategy::_handleMatchNoDedup() {
        DiskLoc loc = _cursor->currLoc();
        _scanAndOrder->add( current( false ), _parsedQuery.showDiskLoc() ? &loc : 0 );
        return false;
    }

    int ReorderBuildStrategy::rewriteMatches() {
        cc().curop()->debug().scanAndOrder = true;
        int ret = 0;
        _scanAndOrder->fill( _buf, _parsedQuery.getFields(), ret );
        return ret;
    }
    
    ScanAndOrder *
    ReorderBuildStrategy::newScanAndOrder( const QueryPlan::Summary &queryPlan ) const {
        verify( 16078, !_parsedQuery.getOrder().isEmpty() );
        verify( 16079, _cursor->ok() );
        const FieldRangeSet *fieldRangeSet = 0;
        if ( queryPlan.valid() ) {
            fieldRangeSet = queryPlan._fieldRangeSetMulti.get();
        }
        else {
            verify( 16080, _queryOptimizerCursor );
            fieldRangeSet = _queryOptimizerCursor->initialFieldRangeSet();
        }
        verify( 16084, fieldRangeSet );
        return new ScanAndOrder( _parsedQuery.getSkip(),
                                _parsedQuery.getNumToReturn(),
                                _parsedQuery.getOrder(),
                                *fieldRangeSet );
    }
    
    HybridBuildStrategy::HybridBuildStrategy( const ParsedQuery &parsedQuery,
                                             const shared_ptr<QueryOptimizerCursor> &cursor,
                                             BufBuilder &buf ) :
    ResponseBuildStrategy( parsedQuery, cursor, buf, QueryPlan::Summary() ),
    _orderedBuild( _parsedQuery, _cursor, _buf, QueryPlan::Summary() ),
    _reorderBuild( _parsedQuery, _cursor, _buf, QueryPlan::Summary() ) {
    }
    
    bool HybridBuildStrategy::handleMatch() {
        if ( !_queryOptimizerCursor->currentPlanScanAndOrderRequired() ) {
            return _orderedBuild.handleMatch();
        }
        handleReorderMatch();
        return false;
    }
    
    void HybridBuildStrategy::handleReorderMatch() {
        DiskLoc loc = _cursor->currLoc();
        if ( _scanAndOrderDups.getsetdup( loc ) ) {
            return;
        }
        try {
            _reorderBuild._handleMatchNoDedup();
        } catch ( const UserException &e ) {
            if ( e.getCode() == ScanAndOrderMemoryLimitExceededAssertionCode ) {
                if ( _queryOptimizerCursor->runningInitialCachedPlan() ) {
                    _queryOptimizerCursor->clearIndexesForPatterns();
                    throw QueryRetryException();
                }
                else if ( _queryOptimizerCursor->runningInitialInOrderPlan() ) {
                    _queryOptimizerCursor->abortOutOfOrderPlans();
                    return;
                }
            }
            throw;
        }            
    }
    
    int HybridBuildStrategy::rewriteMatches() {
        if ( !_queryOptimizerCursor->completePlanOfHybridSetScanAndOrderRequired() ) {
            return _orderedBuild.rewriteMatches();
        }
        resetBuf();
        return _reorderBuild.rewriteMatches();
    }
    
    void HybridBuildStrategy::finishedFirstBatch() {
        _queryOptimizerCursor->abortOutOfOrderPlans();
    }
    
    QueryResponseBuilder::QueryResponseBuilder( const ParsedQuery &parsedQuery,
                                               const shared_ptr<Cursor> &cursor,
                                               const QueryPlan::Summary &queryPlan,
                                               const BSONObj &oldPlan ) :
    _parsedQuery( parsedQuery ),
    _cursor( cursor ),
    _queryOptimizerCursor( dynamic_pointer_cast<QueryOptimizerCursor>( _cursor ) ),
    _buf( 32768 ), // TODO be smarter here
    _chunkManager( newChunkManager() ),
    _explain( newExplainRecordingStrategy( queryPlan, oldPlan ) ),
    _builder( newResponseBuildStrategy( queryPlan ) ),
    _bufferedMatches() {
        _builder->resetBuf();
    }

    bool QueryResponseBuilder::addMatch() {
        if ( !currentMatches() ) {
            return false;
        }
        if ( !chunkMatches() ) {
            return false;
        }
        bool bufferedMatch = _builder->handleMatch();
        _explain->noteIterate( bufferedMatch, true, false );
        if ( bufferedMatch ) {
            ++_bufferedMatches;
        }
        return true;
    }

    void QueryResponseBuilder::noteYield() {
        _explain->noteYield();
    }

    bool QueryResponseBuilder::enoughForFirstBatch() const {
        return _parsedQuery.enoughForFirstBatch( _bufferedMatches, _buf.len() );
    }

    bool QueryResponseBuilder::enoughTotalResults() const {
        if ( _parsedQuery.isExplain() ) {
            return _parsedQuery.enough( _bufferedMatches ) && !_parsedQuery.wantMore();
        }
        return ( _parsedQuery.enough( _bufferedMatches ) ||
                _buf.len() >= MaxBytesToReturnToClientAtOnce );
    }

    void QueryResponseBuilder::finishedFirstBatch() {
        _builder->finishedFirstBatch();
    }

    long long QueryResponseBuilder::handoff( Message &result ) {
        int rewriteCount = _builder->rewriteMatches();
        if ( rewriteCount != -1 ) {
            _bufferedMatches = rewriteCount;
        }
        if ( _parsedQuery.isExplain() ) {
            shared_ptr<ExplainQueryInfo> explainInfo = _explain->doneQueryInfo();
            if ( rewriteCount != -1 ) {
                explainInfo->reviseN( rewriteCount );
            }
            _builder->resetBuf();
            fillQueryResultFromObj( _buf, 0, explainInfo->bson() );
            result.appendData( _buf.buf(), _buf.len() );
            _buf.decouple();
            return 1;
        }
        if ( _buf.len() > 0 ) {
            result.appendData( _buf.buf(), _buf.len() );
            _buf.decouple();
        }
        return _bufferedMatches;
    }

    ShardChunkManagerPtr QueryResponseBuilder::newChunkManager() const {
        if ( !shardingState.needShardChunkManager( _parsedQuery.ns() ) ) {
            return ShardChunkManagerPtr();
        }
        return shardingState.getShardChunkManager( _parsedQuery.ns() );
    }

    shared_ptr<ExplainRecordingStrategy> QueryResponseBuilder::newExplainRecordingStrategy
    ( const QueryPlan::Summary &queryPlan, const BSONObj &oldPlan ) const {
        if ( !_parsedQuery.isExplain() ) {
            return shared_ptr<ExplainRecordingStrategy>( new NoExplainStrategy() );
        }
        ExplainQueryInfo::AncillaryInfo ancillaryInfo;
        ancillaryInfo._oldPlan = oldPlan;
        if ( _queryOptimizerCursor ) {
            return shared_ptr<ExplainRecordingStrategy>
            ( new QueryOptimizerCursorExplainStrategy( ancillaryInfo, _queryOptimizerCursor ) );
        }
        shared_ptr<ExplainRecordingStrategy> ret
        ( new SimpleCursorExplainStrategy( ancillaryInfo, _cursor ) );
        ret->notePlan( queryPlan.valid() && queryPlan._scanAndOrderRequired,
                      queryPlan._keyFieldsOnly );
        return ret;
    }

    shared_ptr<ResponseBuildStrategy> QueryResponseBuilder::newResponseBuildStrategy
    ( const QueryPlan::Summary &queryPlan ) {
        bool unordered = _parsedQuery.getOrder().isEmpty();
        bool empty = !_cursor->ok();
        bool singlePlan = !_queryOptimizerCursor;
        bool singleOrderedPlan =
        singlePlan && ( !queryPlan.valid() || !queryPlan._scanAndOrderRequired );
        QueryOptimizerCursor::CandidatePlans queryOptimizerPlans;
        if ( _queryOptimizerCursor ) {
            queryOptimizerPlans = _queryOptimizerCursor->initialCandidatePlans();
        }
        if ( unordered ||
            empty ||
            singleOrderedPlan ||
            ( !singlePlan && !queryOptimizerPlans.mayRunOutOfOrderPlan() ) ) {
            return shared_ptr<ResponseBuildStrategy>
            ( new OrderedBuildStrategy( _parsedQuery, _cursor, _buf, queryPlan ) );
        }
        if ( singlePlan ||
            !queryOptimizerPlans.mayRunInOrderPlan() ) {
            return shared_ptr<ResponseBuildStrategy>
            ( new ReorderBuildStrategy( _parsedQuery, _cursor, _buf, queryPlan ) );
        }
        return shared_ptr<ResponseBuildStrategy>
        ( new HybridBuildStrategy( _parsedQuery, _queryOptimizerCursor, _buf ) );
    }

    bool QueryResponseBuilder::currentMatches() {
        MatchDetails details;
        if ( _cursor->currentMatches( &details ) ) {
            return true;
        }
        _explain->noteIterate( false, details._loadedObject, false );
        return false;
    }

    bool QueryResponseBuilder::chunkMatches() {
        if ( !_chunkManager ) {
            return true;
        }
        // TODO: should make this covered at some point
        if ( _chunkManager->belongsToMe( _cursor->current() ) ) {
            return true;
        }
        _explain->noteIterate( false, true, true );
        return false;
    }
    
    /**
     * Run a query with a cursor provided by the query optimizer, or FindingStartCursor.
     * @yields the db lock.
     */
    const char *queryWithQueryOptimizer( Message &m, int queryOptions, const char *ns,
                                        const BSONObj &jsobj, CurOp& curop,
                                        const BSONObj &query, const BSONObj &order,
                                        const shared_ptr<ParsedQuery> &pq_shared,
                                        const BSONObj &oldPlan,
                                        const ConfigVersion &shardingVersionAtStart,
                                        Message &result ) {

        const ParsedQuery &pq( *pq_shared );
        shared_ptr<Cursor> cursor;
        QueryPlan::Summary queryPlan;
        
        if ( pq.hasOption( QueryOption_OplogReplay ) ) {
            cursor = FindingStartCursor::getCursor( ns, query, order );
        }
        else {
            cursor =
            NamespaceDetailsTransient::getCursor( ns, query, order, QueryPlanSelectionPolicy::any(),
                                                 0, &pq, &queryPlan );
        }
        verify( 16081, cursor );
        
        QueryResponseBuilder queryResponseBuilder( pq, cursor, queryPlan, oldPlan );
        bool saveClientCursor = false;
        const char *exhaust = 0;
        OpTime slaveReadTill;
        ClientCursor::CleanupPointer ccPointer;
        ccPointer.reset( new ClientCursor( QueryOption_NoCursorTimeout, cursor, ns ) );
        
        for( ; cursor->ok(); cursor->advance() ) {

            bool yielded = false;
            if ( !ccPointer->yieldSometimes( ClientCursor::MaybeCovered, &yielded ) ||
                !cursor->ok() ) {
                cursor.reset();
                queryResponseBuilder.noteYield();
                // !!! TODO The queryResponseBuilder still holds cursor.  Currently it will not do
                // anything unsafe with the cursor in handoff(), but this is very fragile.
                break;
            }

            if ( yielded ) {
                queryResponseBuilder.noteYield();
            }
            
            if ( pq.getMaxScan() && cursor->nscanned() > pq.getMaxScan() ) {
                break;
            }
            
            if ( !queryResponseBuilder.addMatch() ) {
                continue;
            }
            
            // Note slave's position in the oplog.
            if ( pq.hasOption( QueryOption_OplogReplay ) ) {
                BSONObj current = cursor->current();
                BSONElement e = current["ts"];
                if ( e.type() == Date || e.type() == Timestamp ) {
                    slaveReadTill = e._opTime();
                }
            }
            
            if ( !cursor->supportGetMore() || pq.isExplain() ) {
                if ( queryResponseBuilder.enoughTotalResults() ) {
                    break;
                }
            }
            else if ( queryResponseBuilder.enoughForFirstBatch() ) {
                // if only 1 requested, no cursor saved for efficiency...we assume it is findOne()
                if ( pq.wantMore() && pq.getNumToReturn() != 1 ) {
                    queryResponseBuilder.finishedFirstBatch();
                    if ( cursor->advance() ) {
                        saveClientCursor = true;
                    }
                }
                break;
            }
        }
        
        if ( cursor ) {
            if ( pq.hasOption( QueryOption_CursorTailable ) && pq.getNumToReturn() != 1 ) {
                cursor->setTailable();
            }
            
            // If the tailing request succeeded.
            if ( cursor->tailable() ) {
                saveClientCursor = true;
            }
        }
        
        if ( shardingState.getVersion( ns ) != shardingVersionAtStart ) {
            // if the version changed during the query
            // we might be missing some data
            // and its safe to send this as mongos can resend
            // at this point
            throw SendStaleConfigException( ns , "version changed during initial query", shardingVersionAtStart, shardingState.getVersion( ns ) );
        }

        long long nReturned = queryResponseBuilder.handoff( result );

        ccPointer.reset();
        long long cursorid = 0;
        if ( saveClientCursor ) {
            // Create a new ClientCursor, with a default timeout.
            ccPointer.reset( new ClientCursor( queryOptions, cursor, ns,
                                              jsobj.getOwned() ) );
            cursorid = ccPointer->cursorid();
            DEV tlog(2) << "query has more, cursorid: " << cursorid << endl;
            if ( cursor->supportYields() ) {
                ClientCursor::YieldData data;
                ccPointer->prepareToYield( data );
            }
            else {
                ccPointer->updateLocation();
            }
            
            // !!! Save the original message buffer, so it can be referenced in getMore.
            ccPointer->originalMessage = m;

            // Save slave's position in the oplog.
            if ( pq.hasOption( QueryOption_OplogReplay ) && !slaveReadTill.isNull() ) {
                ccPointer->slaveReadTill( slaveReadTill );
            }
            
            if ( !ccPointer->ok() && ccPointer->c()->tailable() ) {
                DEV tlog() << "query has no more but tailable, cursorid: " << cursorid << endl;
            }
            
            if( queryOptions & QueryOption_Exhaust ) {
                exhaust = ns;
                curop.debug().exhaust = true;
            }
            
            // Set attributes for getMore.
            ccPointer->setChunkManager( queryResponseBuilder.chunkManager() );
            ccPointer->setPos( nReturned );
            ccPointer->pq = pq_shared;
            ccPointer->fields = pq.getFieldPtr();
            ccPointer.release();
        }
        
        QueryResult *qr = (QueryResult *) result.header();
        qr->cursorId = cursorid;
        curop.debug().cursorid = ( cursorid == 0 ? -1 : qr->cursorId );
        qr->setResultFlagsToOk();
        // qr->len is updated automatically by appendData()
        curop.debug().responseLength = qr->len;
        qr->setOperation(opReply);
        qr->startingFrom = 0;
        qr->nReturned = nReturned;
        
        int duration = curop.elapsedMillis();
        bool dbprofile = curop.shouldDBProfile( duration );
        if ( dbprofile || duration >= cmdLine.slowMS ) {
            curop.debug().nscanned = (int)( cursor ? cursor->nscanned() : 0 );
            curop.debug().ntoskip = pq.getSkip();
        }
        curop.debug().nreturned = nReturned;

        return exhaust;
    }
    
    /**
     * Run a query -- includes checking for and running a Command.
     * @return points to ns if exhaust mode. 0=normal mode
     * @locks the db mutex for reading (and potentially for writing temporarily to create a new db).
     * @yields the db mutex periodically after acquiring it.
     * @asserts on scan and order memory exhaustion and other cases.
     */
    const char *runQuery(Message& m, QueryMessage& q, CurOp& curop, Message &result) {
        shared_ptr<ParsedQuery> pq_shared( new ParsedQuery(q) );
        ParsedQuery& pq( *pq_shared );
        BSONObj jsobj = q.query;
        int queryOptions = q.queryOptions;
        const char *ns = q.ns;

        if( logLevel >= 2 )
            log() << "runQuery called " << ns << " " << jsobj << endl;

        curop.debug().ns = ns;
        curop.debug().ntoreturn = pq.getNumToReturn();
        curop.debug().query = jsobj;
        curop.setQuery(jsobj);

        // Run a command.
        
        if ( pq.couldBeCommand() ) {
            BufBuilder bb;
            bb.skip(sizeof(QueryResult));
            BSONObjBuilder cmdResBuf;
            if ( runCommands(ns, jsobj, curop, bb, cmdResBuf, false, queryOptions) ) {
                curop.debug().iscommand = true;
                curop.debug().query = jsobj;
                curop.markCommand();

                auto_ptr< QueryResult > qr;
                qr.reset( (QueryResult *) bb.buf() );
                bb.decouple();
                qr->setResultFlagsToOk();
                qr->len = bb.len();
                curop.debug().responseLength = bb.len();
                qr->setOperation(opReply);
                qr->cursorId = 0;
                qr->startingFrom = 0;
                qr->nReturned = 1;
                result.setData( qr.release(), true );
            }
            else {
                uasserted(13530, "bad or malformed command request?");
            }
            return 0;
        }

        bool explain = pq.isExplain();
        BSONObj order = pq.getOrder();
        BSONObj query = pq.getFilter();

        /* The ElemIter will not be happy if this isn't really an object. So throw exception
           here when that is true.
           (Which may indicate bad data from client.)
        */
        if ( query.objsize() == 0 ) {
            out() << "Bad query object?\n  jsobj:";
            out() << jsobj.toString() << "\n  query:";
            out() << query.toString() << endl;
            uassert( 10110 , "bad query object", false);
        }

        Client::ReadContext ctx( ns , dbpath ); // read locks
        const ConfigVersion shardingVersionAtStart = shardingState.getVersion( ns );

        replVerifyReadsOk(&pq);

        if ( pq.hasOption( QueryOption_CursorTailable ) ) {
            NamespaceDetails *d = nsdetails( ns );
            uassert( 13051, "tailable cursor requested on non capped collection", d && d->capped );
            const BSONObj nat1 = BSON( "$natural" << 1 );
            if ( order.isEmpty() ) {
                order = nat1;
            }
            else {
                uassert( 13052, "only {$natural:1} order allowed for tailable cursor", order == nat1 );
            }
        }

        // Run a simple id query.
        
        if ( ! (explain || pq.showDiskLoc()) && isSimpleIdQuery( query ) && !pq.hasOption( QueryOption_CursorTailable ) ) {

            int n = 0;
            bool nsFound = false;
            bool indexFound = false;

            BSONObj resObject;
            Client& c = cc();
            bool found = Helpers::findById( c, ns , query , resObject , &nsFound , &indexFound );
            if ( nsFound == false || indexFound == true ) {
                
                if ( shardingState.needShardChunkManager( ns ) ) {
                    ShardChunkManagerPtr m = shardingState.getShardChunkManager( ns );
                    if ( m && ! m->belongsToMe( resObject ) ) {
                        // I have something this _id
                        // but it doesn't belong to me
                        // so return nothing
                        resObject = BSONObj();
                        found = false;
                    }
                }

                BufBuilder bb(sizeof(QueryResult)+resObject.objsize()+32);
                bb.skip(sizeof(QueryResult));
                
                curop.debug().idhack = true;
                if ( found ) {
                    n = 1;
                    fillQueryResultFromObj( bb , pq.getFields() , resObject );
                }
                auto_ptr< QueryResult > qr;
                qr.reset( (QueryResult *) bb.buf() );
                bb.decouple();
                qr->setResultFlagsToOk();
                qr->len = bb.len();
                
                curop.debug().responseLength = bb.len();
                qr->setOperation(opReply);
                qr->cursorId = 0;
                qr->startingFrom = 0;
                qr->nReturned = n;
                result.setData( qr.release(), true );
                return NULL;
            }
        }
        
        // Run a regular query.
        
        BSONObj oldPlan;
        if ( explain && ! pq.hasIndexSpecifier() ) {
            MultiPlanScanner mps( ns, query, shared_ptr<Projection>(), order );
            if ( mps.usingCachedPlan() ) {
                oldPlan =
                mps.oldExplain().firstElement().embeddedObject()
                .firstElement().embeddedObject().getOwned();
            }
        }

        // In some cases the query may be retried if there is an in memory sort size assertion.
        for( int retry = 0; retry < 2; ++retry ) {
            try {
                return queryWithQueryOptimizer( m, queryOptions, ns, jsobj, curop, query, order,
                                               pq_shared, oldPlan, shardingVersionAtStart, result );
            } catch ( const QueryRetryException & ) {
                verify( 16088, retry == 0 );
            }
        }
        verify( 16082, false );
        return 0;
    }

} // namespace mongo
