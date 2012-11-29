/* Copyright 2010 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/mutable/mutable_bson_internal.h"

namespace mongo {
namespace mutablebson {

    //
    // ElementRep implementation
    //

    ElementRep::ElementRep() :
        _type(mongo::Undefined),
        _nameref(NULL_REF),
        _parent(EMPTY_REP) {

        _child._left = EMPTY_REP;
        _child._right = EMPTY_REP;
        _sibling._left = EMPTY_REP;
        _sibling._right = EMPTY_REP;
    }

    ElementRep::ElementRep(int32_t type) :
        _type(type),
        _nameref(NULL_REF),
        _parent(EMPTY_REP) {

        _child._left = EMPTY_REP;
        _child._right = EMPTY_REP;
        _sibling._left = EMPTY_REP;
        _sibling._right = EMPTY_REP;
    }

    ElementRep::ElementRep(int32_t type, uint32_t nameref) :
        _type(type),
        _nameref(nameref),
        _parent(EMPTY_REP) {

        _child._left = EMPTY_REP;
        _child._right = EMPTY_REP;
        _sibling._left = EMPTY_REP;
        _sibling._right = EMPTY_REP;
    }

    ElementRep::ElementRep(int32_t type, uint32_t nameref, ValueType value) :
        _type(type),
        _nameref(nameref),
        _value(value),
        _parent(EMPTY_REP) {

        _sibling._left = EMPTY_REP;
        _sibling._right = EMPTY_REP;
    }

    ElementRep::ElementRep(int32_t type, uint32_t nameref, ValueType value, uint32_t parent) :
        _type(type),
        _nameref(nameref),
        _value(value),
        _parent(parent) {

        _sibling._left = EMPTY_REP;
        _sibling._right = EMPTY_REP;
    }

    ElementRep::~ElementRep() {
    }

    void ElementRep::clearSiblings() {
        _sibling._left = EMPTY_REP;
        _sibling._right = EMPTY_REP;
    }

    void ElementRep::clearParent() {
        _parent = EMPTY_REP;
    }

    //
    // ElementVector implementation
    //

    ElementVector::ElementVector() {
    }

    ElementVector::~ElementVector() {
    }

    const ElementRep& ElementVector::operator[](uint32_t n) const {
        return _vec[n];
    }

    ElementRep& ElementVector::operator[](uint32_t n) {
        return _vec[n];
    }

    uint32_t ElementVector::size() const {
        return _vec.size();
    }

    void ElementVector::push_back(const ElementRep& rep) {
        _vec.push_back(rep);
    }

} // namespace mutablebson
} // namespace mongo
