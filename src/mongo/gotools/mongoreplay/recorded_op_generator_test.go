package mongoreplay

import (
	"fmt"
	"time"

	mgo "github.com/10gen/llmgo"
	"github.com/10gen/llmgo/bson"
)

// recordedOpGenerator maintains a pair of connection stubs and channel to allow
// ops to be generated by the driver and passed to a channel
type recordedOpGenerator struct {
	session          *SessionStub
	serverConnection *ConnStub
	opChan           chan *RecordedOp
}

func newRecordedOpGenerator() *recordedOpGenerator {
	session := SessionStub{}
	var serverConnection ConnStub
	serverConnection, session.connection = newTwoSidedConn()
	opChan := make(chan *RecordedOp, 1000)

	return &recordedOpGenerator{
		session:          &session,
		serverConnection: &serverConnection,
		opChan:           opChan,
	}
}

// pushDriverRequestOps takes the pair of ops that the driver generator (the
// nonce and the main op) and places them in the channel
func (generator *recordedOpGenerator) pushDriverRequestOps(recordedOp *RecordedOp) {
	generator.opChan <- recordedOp
}

type driverRequestOps struct {
	nonce *RecordedOp
	op    *RecordedOp
}

func getmoreArgsHelper(cursorID int64, limit int32) bson.D {
	var getmoreArgs bson.D
	if limit > 0 {
		getmoreArgs = bson.D{{"getMore", cursorID}, {"collection", testCollection}, {"batchSize", limit}}
	} else {
		getmoreArgs = bson.D{{"getMore", cursorID}, {"collection", testCollection}}
	}
	return getmoreArgs
}

func findArgsHelper(filter interface{}, limit int32) bson.D {
	var findArgs bson.D
	if limit > 0 {
		findArgs = bson.D{{"find", testCollection}, {"filter", filter}, {"batchSize", limit}}
	} else {
		findArgs = bson.D{{"find", testCollection}, {"filter", filter}}
	}
	return findArgs
}

func commandReplyArgsHelper(cursorID int64) interface{} {
	commandReply := &struct {
		Cursor struct {
			ID int64 `bson:"id"`
		} `bson:"cursor"`
	}{}
	commandReply.Cursor.ID = cursorID
	return commandReply
}

// generateInsert creates a RecordedOp insert using the given documents and
// pushes it to the recordedOpGenerator's channel to be executed when Play() is
// called
func (generator *recordedOpGenerator) generateInsert(docs []interface{}) error {
	insert := mgo.InsertOp{Collection: fmt.Sprintf("%s.%s", testDB, testCollection),
		Documents: docs,
		Flags:     0,
	}
	recordedOp, err := generator.fetchRecordedOpsFromConn(&insert)
	if err != nil {
		return err
	}
	generator.pushDriverRequestOps(recordedOp)
	return nil

}

func (generator *recordedOpGenerator) generateGetLastError() error {
	query := bson.D{{"getLastError", 1}}
	getLastError := mgo.QueryOp{
		Collection: "admin.$cmd",
		Query:      query,
		Limit:      -1,
		Flags:      0,
	}
	recordedOp, err := generator.fetchRecordedOpsFromConn(&getLastError)
	if err != nil {
		return err
	}
	generator.pushDriverRequestOps(recordedOp)
	return nil

}

func (generator *recordedOpGenerator) generateInsertHelper(name string, startFrom, numInserts int) error {
	for i := 0; i < numInserts; i++ {
		doc := testDoc{
			Name:           name,
			DocumentNumber: i + startFrom,
			Success:        true,
		}
		err := generator.generateInsert([]interface{}{doc})
		if err != nil {
			return err
		}
	}
	return nil
}

// generateQuery creates a RecordedOp query using the given selector, limit, and
// requestID, and pushes it to the recordedOpGenerator's channel to be executed
// when Play() is called
func (generator *recordedOpGenerator) generateQuery(querySelection interface{}, limit int32, requestID int32) error {
	query := mgo.QueryOp{
		Flags:      0,
		HasOptions: true,
		Skip:       0,
		Limit:      limit,
		Selector:   bson.D{},
		Query:      querySelection,
		Collection: fmt.Sprintf("%s.%s", testDB, testCollection),
		Options:    mgo.QueryWrapper{},
	}

	recordedOp, err := generator.fetchRecordedOpsFromConn(&query)
	if err != nil {
		return err
	}
	recordedOp.RawOp.Header.RequestID = requestID
	generator.pushDriverRequestOps(recordedOp)
	return nil
}

func (generator *recordedOpGenerator) generateCommandOpInsertHelper(name string, startFrom, numInserts int) error {
	for i := 0; i < numInserts; i++ {
		doc := testDoc{
			Name:           name,
			DocumentNumber: i + startFrom,
			Success:        true,
		}
		commandArgs := bson.D{{"documents", []testDoc{doc}}, {"insert", testCollection}}
		err := generator.generateCommandOp("insert", commandArgs, 0)
		if err != nil {
			return err
		}
	}
	return nil
}

func (generator *recordedOpGenerator) generateCommandFind(filter interface{}, limit int32, requestID int32) error {
	findArgs := findArgsHelper(filter, limit)
	return generator.generateCommandOp("find", findArgs, requestID)
}

func (generator *recordedOpGenerator) generateCommandGetMore(cursorID int64, limit int32) error {
	getmoreArgs := getmoreArgsHelper(cursorID, limit)
	return generator.generateCommandOp("getMore", getmoreArgs, 0)
}
func (generator *recordedOpGenerator) generateCommandReply(responseTo int32, cursorID int64) error {
	commandReply := commandReplyArgsHelper(cursorID)

	reply := mgo.CommandReplyOp{
		Metadata:     &struct{}{},
		CommandReply: commandReply,
		OutputDocs:   []interface{}{},
	}

	recordedOp, err := generator.fetchRecordedOpsFromConn(&reply)
	if err != nil {
		return err
	}

	recordedOp.RawOp.Header.ResponseTo = responseTo
	tempEnd := recordedOp.SrcEndpoint
	recordedOp.SrcEndpoint = recordedOp.DstEndpoint
	recordedOp.DstEndpoint = tempEnd
	generator.pushDriverRequestOps(recordedOp)
	return nil
}

func (generator *recordedOpGenerator) generateCommandOp(commandName string, commandArgs bson.D, requestID int32) error {
	command := mgo.CommandOp{
		Database:    testDB,
		CommandName: commandName,
		Metadata:    bson.D{},
		CommandArgs: commandArgs,
		InputDocs:   make([]interface{}, 0),
	}

	recordedOp, err := generator.fetchRecordedOpsFromConn(&command)
	if err != nil {
		return err
	}
	recordedOp.RawOp.Header.RequestID = requestID
	generator.pushDriverRequestOps(recordedOp)
	return nil
}

// generateGetMore creates a RecordedOp getMore using the given cursorID and
// limit and pushes it to the recordedOpGenerator's channel to be executed when
// Play() is called
func (generator *recordedOpGenerator) generateGetMore(cursorID int64, limit int32) error {
	getMore := mgo.GetMoreOp{
		Collection: fmt.Sprintf("%s.%s", testDB, testCollection),
		CursorId:   cursorID,
		Limit:      limit,
	}

	recordedOp, err := generator.fetchRecordedOpsFromConn(&getMore)
	if err != nil {
		return err
	}
	generator.pushDriverRequestOps(recordedOp)
	return nil
}

// generateReply creates a RecordedOp reply using the given responseTo,
// cursorID, and firstDOc, and pushes it to the recordedOpGenerator's channel to
// be executed when Play() is called
func (generator *recordedOpGenerator) generateReply(responseTo int32, cursorID int64) error {
	reply := mgo.ReplyOp{
		Flags:     0,
		CursorId:  cursorID,
		FirstDoc:  0,
		ReplyDocs: 5,
	}

	recordedOp, err := generator.fetchRecordedOpsFromConn(&reply)
	if err != nil {
		return err
	}

	recordedOp.RawOp.Header.ResponseTo = responseTo
	SetInt64(recordedOp.RawOp.Body, 4, cursorID) // change the cursorID field in the RawOp.Body
	tempEnd := recordedOp.SrcEndpoint
	recordedOp.SrcEndpoint = recordedOp.DstEndpoint
	recordedOp.DstEndpoint = tempEnd
	generator.pushDriverRequestOps(recordedOp)
	return nil
}

func (generator *recordedOpGenerator) generateMsgOpInsertHelper(name string, startFrom, numInserts int) error {
	for i := 0; i < numInserts; i++ {
		doc := testDoc{
			Name:           name,
			DocumentNumber: i + startFrom,
			Success:        true,
		}
		err := generator.generateMsgOpAgainstCollection("insert", "documents", []interface{}{doc}, 0)
		if err != nil {
			return err
		}
	}
	return nil
}

func (generator *recordedOpGenerator) generateMsgOpGetMore(cursorID int64, limit int32) error {
	getmoreArgs := getmoreArgsHelper(cursorID, limit)
	getmoreArgs = append(getmoreArgs, bson.DocElem{"$db", testDB})
	section := mgo.MsgSection{
		PayloadType: mgo.MsgPayload0,
		Data:        getmoreArgs,
	}
	return generator.generateMsgOp([]mgo.MsgSection{section}, 0)
}

func (generator *recordedOpGenerator) generateMsgOpFind(filter interface{}, limit, requestID int32) error {
	findArgs := findArgsHelper(filter, limit)
	findArgs = append(findArgs, bson.DocElem{"$db", testDB})
	section := mgo.MsgSection{
		PayloadType: mgo.MsgPayload0,
		Data:        findArgs,
	}
	return generator.generateMsgOp([]mgo.MsgSection{section}, requestID)
}

func (generator *recordedOpGenerator) generateMsgOpReply(responseTo int32, cursorID int64) error {
	commandReply := commandReplyArgsHelper(cursorID)
	commandReplyAsSlice, err := bson.Marshal(commandReply)
	if err != nil {
		return err
	}
	commandReplyAsRaw := &bson.Raw{}

	err = bson.Unmarshal(commandReplyAsSlice, commandReplyAsRaw)
	if err != nil {
		return err
	}

	replySection := mgo.MsgSection{
		PayloadType: mgo.MsgPayload0,
		Data:        commandReplyAsRaw,
	}

	msgOpReply := mgo.MsgOp{
		Sections: []mgo.MsgSection{replySection},
	}

	recordedOp, err := generator.fetchRecordedOpsFromConn(&msgOpReply)
	if err != nil {
		return err
	}

	recordedOp.RawOp.Header.ResponseTo = responseTo
	tempEnd := recordedOp.SrcEndpoint
	recordedOp.SrcEndpoint = recordedOp.DstEndpoint
	recordedOp.DstEndpoint = tempEnd
	generator.pushDriverRequestOps(recordedOp)
	return nil
}

func (generator *recordedOpGenerator) generateMsgOpAgainstCollection(opName, identifier string, docs []interface{}, requestID int32) error {
	section0 := mgo.MsgSection{
		PayloadType: mgo.MsgPayload0,
		Data:        bson.D{{opName, testCollection}, {"$db", testDB}},
	}
	p1 := mgo.PayloadType1{
		Identifier: identifier,
		Docs:       docs,
	}
	p1Size, err := p1.CalculateSize()
	if err != nil {
		return err
	}
	p1.Size = p1Size
	section1 := mgo.MsgSection{
		PayloadType: mgo.MsgPayload1,
		Data:        p1,
	}
	return generator.generateMsgOp([]mgo.MsgSection{section0, section1}, requestID)
}

func (generator *recordedOpGenerator) generateMsgOp(sections []mgo.MsgSection, requestID int32) error {
	opMsg := mgo.MsgOp{
		Sections: sections,
	}
	recordedOp, err := generator.fetchRecordedOpsFromConn(&opMsg)
	if err != nil {
		return err
	}
	recordedOp.RawOp.Header.RequestID = requestID
	generator.pushDriverRequestOps(recordedOp)
	return nil
}

// generateKillCursorsOp creates a RecordedOp killCursors using the given
// cursorIDs and pushes it to the recordedOpGenerator's channel to be executed
// when Play() is called
func (generator *recordedOpGenerator) generateKillCursors(cursorIDs []int64) error {
	killCursors := mgo.KillCursorsOp{
		CursorIds: cursorIDs,
	}

	recordedOp, err := generator.fetchRecordedOpsFromConn(&killCursors)
	if err != nil {
		return err
	}
	generator.pushDriverRequestOps(recordedOp)
	return nil
}

// fetchRecordedOpsFromConn runs the created mgo op through mgo and fetches its
// result from the stubbed connection. In the case that a connection has not
// been used before it reads two ops from the connection, the first being the
// 'getNonce' request generated by the driver
func (generator *recordedOpGenerator) fetchRecordedOpsFromConn(op interface{}) (*RecordedOp, error) {
	socket, err := generator.session.AcquireSocketPrivate(true)
	if err != nil {
		return nil, fmt.Errorf("AcquireSocketPrivate: %v\n", err)
	}
	err = socket.Query(op)
	if err != nil {
		return nil, fmt.Errorf("Socket.Query: %v\n", err)
	}
	msg, err := ReadHeader(generator.serverConnection)
	if err != nil {
		return nil, fmt.Errorf("ReadHeader Error: %v\n", err)
	}
	result := RawOp{Header: *msg}
	result.Body = make([]byte, MsgHeaderLen)
	result.FromReader(generator.serverConnection)

	recordedOp := &RecordedOp{RawOp: result, Seen: &PreciseTime{testTime}, SrcEndpoint: "a", DstEndpoint: "b", PlayedAt: &PreciseTime{}}

	testTime = testTime.Add(time.Millisecond * 2)
	return recordedOp, nil
}
