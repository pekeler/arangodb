////////////////////////////////////////////////////////////////////////////////
/// @brief base transaction wrapper
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2004-2013 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Jan Steemann
/// @author Copyright 2011-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef TRIAGENS_UTILS_TRANSACTION_H
#define TRIAGENS_UTILS_TRANSACTION_H 1

#include "Basics/Common.h"

#include "Cluster/ServerState.h"

#include "VocBase/barrier.h"
#include "VocBase/collection.h"
#include "VocBase/document-collection.h"
#include "VocBase/edge-collection.h"
#include "VocBase/transaction.h"
#include "VocBase/update-policy.h"
#include "VocBase/vocbase.h"
#include "VocBase/voc-shaper.h"
#include "VocBase/voc-types.h"

#include "BasicsC/logging.h"
#include "BasicsC/random.h"
#include "BasicsC/tri-strings.h"

#include "Utils/CollectionNameResolver.h"
#include "Utils/DocumentHelper.h"

namespace triagens {
  namespace arango {

// -----------------------------------------------------------------------------
// --SECTION--                                             class TransactionBase
// -----------------------------------------------------------------------------

    class TransactionBase {

////////////////////////////////////////////////////////////////////////////////
/// @brief Transaction base, every transaction class has to inherit from here
////////////////////////////////////////////////////////////////////////////////

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief constructor
////////////////////////////////////////////////////////////////////////////////

        TransactionBase () {
          TRI_ASSERT(_numberTrxInScope >= 0);
          TRI_ASSERT(_numberTrxInScope == _numberTrxActive);
          _numberTrxInScope++;
        }

        explicit TransactionBase (bool standalone) {
          TRI_ASSERT(_numberTrxInScope >= 0);
          TRI_ASSERT(_numberTrxInScope == _numberTrxActive);
          _numberTrxInScope++;
          if (standalone) {
            _numberTrxActive++;
          }
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief destructor
////////////////////////////////////////////////////////////////////////////////

        virtual ~TransactionBase () {
          TRI_ASSERT(_numberTrxInScope > 0);
          _numberTrxInScope--;
          // Note that embedded transactions might have seen a begin()
          // but no abort() or commit(), so _numberTrxActive might
          // be one too big. We simply fix it here:
          _numberTrxActive = _numberTrxInScope;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief assert that a transaction object is in scope in the current thread
////////////////////////////////////////////////////////////////////////////////

        static void assertSomeTrxInScope () {
          TRI_ASSERT(_numberTrxInScope > 0);
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief assert that the innermost transaction object in scope in the 
/// current thread is actually active (between begin() and commit()/abort().
////////////////////////////////////////////////////////////////////////////////

        static void assertCurrentTrxActive () {
          TRI_ASSERT(_numberTrxInScope > 0 &&
                     _numberTrxInScope == _numberTrxActive);
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief the following is for the runtime protection check, number of
/// transaction objects in scope in the current thread
////////////////////////////////////////////////////////////////////////////////

      protected:

        static thread_local int _numberTrxInScope;

////////////////////////////////////////////////////////////////////////////////
/// @brief the following is for the runtime protection check, number of
/// transaction objects in the current thread that are active (between
/// begin and commit()/abort().
////////////////////////////////////////////////////////////////////////////////

        static thread_local int _numberTrxActive;

      };

// -----------------------------------------------------------------------------
// --SECTION--                                                 class Transaction
// -----------------------------------------------------------------------------

      template<typename T>
      class Transaction : public T, public TransactionBase {

////////////////////////////////////////////////////////////////////////////////
/// @brief Transaction
////////////////////////////////////////////////////////////////////////////////

        private:
          Transaction (const Transaction&) = delete;
          Transaction& operator= (const Transaction&) = delete;

// -----------------------------------------------------------------------------
// --SECTION--                                      constructors and destructors
// -----------------------------------------------------------------------------

        public:

////////////////////////////////////////////////////////////////////////////////
/// @brief create the transaction
////////////////////////////////////////////////////////////////////////////////

          Transaction (TRI_vocbase_t* vocbase,
                       TRI_server_id_t generatingServer,
                       bool replicate) :
            T(),
            _setupState(TRI_ERROR_NO_ERROR),
            _nestingLevel(0),
            _errorData(),
            _hints(0),
            _timeout(0.0),
            _waitForSync(false),
            _replicate(replicate),
            _isReal(true),
            _trx(0),
            _vocbase(vocbase),
            _generatingServer(generatingServer) {

            TRI_ASSERT(_vocbase != nullptr);

            if (ServerState::instance()->isCoordinator()) {
              _isReal = false;
            }

            this->setupTransaction();
          }

////////////////////////////////////////////////////////////////////////////////
/// @brief destroy the transaction
////////////////////////////////////////////////////////////////////////////////

          virtual ~Transaction () {
            if (_trx == nullptr) {
              return;
            }
            
            if (isEmbeddedTransaction()) {
              _trx->_nestingLevel--;
            }
            else {
              if (getStatus() == TRI_TRANSACTION_RUNNING) {
                // auto abort a running transaction
                this->abort();
              }

              // free the data associated with the transaction
              freeTransaction();
            }
          }

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

        public:

////////////////////////////////////////////////////////////////////////////////
/// @brief return the registered error data
////////////////////////////////////////////////////////////////////////////////

          std::string const getErrorData () const {
            return _errorData;
          }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the collection name resolver
////////////////////////////////////////////////////////////////////////////////
          
          CollectionNameResolver const* resolver () const {
            CollectionNameResolver const* r = this->getResolver();
            TRI_ASSERT(r != nullptr);
            return r;
          }

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not the transaction is embedded
////////////////////////////////////////////////////////////////////////////////

          inline bool isEmbeddedTransaction () const {
            return (_nestingLevel > 0);
          }

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not shaped json in this trx should be copied
////////////////////////////////////////////////////////////////////////////////

        inline bool mustCopyShapedJson () const {
          if (_trx != nullptr && _trx->_hasOperations) {
            return true;
          }

            return false;
          }

////////////////////////////////////////////////////////////////////////////////
/// @brief get the status of the transaction
////////////////////////////////////////////////////////////////////////////////

          inline TRI_transaction_status_e getStatus () const {
            if (_trx != nullptr) {
              return _trx->_status;
            }

            return TRI_TRANSACTION_UNDEFINED;
          }

////////////////////////////////////////////////////////////////////////////////
/// @brief begin the transaction
////////////////////////////////////////////////////////////////////////////////

          int begin () {
            if (_trx == nullptr) {
              return TRI_ERROR_TRANSACTION_INTERNAL;
            }
            
            if (_setupState != TRI_ERROR_NO_ERROR) {
              return _setupState;
            }

            TRI_ASSERT(_numberTrxActive == _numberTrxInScope - 1);
            _numberTrxActive++;  // Every transaction gets here at most once

            if (! _isReal) {
              if (_nestingLevel == 0) {
                _trx->_status = TRI_TRANSACTION_RUNNING;
              }
              return TRI_ERROR_NO_ERROR;
            }


            int res = TRI_BeginTransaction(_trx, _hints, _nestingLevel);

            return res;
          }

////////////////////////////////////////////////////////////////////////////////
/// @brief commit / finish the transaction
////////////////////////////////////////////////////////////////////////////////

          int commit () {
            if (_trx == nullptr || getStatus() != TRI_TRANSACTION_RUNNING) {
              // transaction not created or not running
              return TRI_ERROR_TRANSACTION_INTERNAL;
            }
            
            if (! _isReal) {
              if (_nestingLevel == 0) {
                _trx->_status = TRI_TRANSACTION_COMMITTED;
              }
              TRI_ASSERT(_numberTrxActive == _numberTrxInScope);
              _numberTrxActive--;  // Every transaction gets here at most once
              return TRI_ERROR_NO_ERROR;
            }

            int res = TRI_CommitTransaction(_trx, _nestingLevel);

            TRI_ASSERT(_numberTrxActive == _numberTrxInScope);
            _numberTrxActive--;  // Every transaction gets here at most once

            return res;
          }

////////////////////////////////////////////////////////////////////////////////
/// @brief abort the transaction
////////////////////////////////////////////////////////////////////////////////

          int abort () {
            if (_trx == nullptr || getStatus() != TRI_TRANSACTION_RUNNING) {
              // transaction not created or not running
              return TRI_ERROR_TRANSACTION_INTERNAL;
            }

            if (! _isReal) {
              if (_nestingLevel == 0) {
                _trx->_status = TRI_TRANSACTION_ABORTED;
              }
              TRI_ASSERT(_numberTrxActive == _numberTrxInScope);
              _numberTrxActive--;  // Every transaction gets here at most once
              return TRI_ERROR_NO_ERROR;
            }

            int res = TRI_AbortTransaction(_trx, _nestingLevel);
            
            TRI_ASSERT(_numberTrxActive == _numberTrxInScope);
            _numberTrxActive--;  // Every transaction gets here at most once

            return res;
          }

////////////////////////////////////////////////////////////////////////////////
/// @brief finish a transaction (commit or abort), based on the previous state
////////////////////////////////////////////////////////////////////////////////

          int finish (const int errorNum) {
            int res;

            if (errorNum == TRI_ERROR_NO_ERROR) {
              // there was no previous error, so we'll commit
              res = this->commit();
            }
            else {
              // there was a previous error, so we'll abort
              this->abort();

              // return original error number
              res = errorNum;
            }
            
            return res;
          }

////////////////////////////////////////////////////////////////////////////////
/// @brief order a barrier for a collection
////////////////////////////////////////////////////////////////////////////////

         TRI_barrier_t* orderBarrier (TRI_transaction_collection_t* trxCollection) {
           TRI_ASSERT(_trx != nullptr);
           TRI_ASSERT(getStatus() == TRI_TRANSACTION_RUNNING);
           TRI_ASSERT(trxCollection->_collection != nullptr);

           TRI_document_collection_t* document = trxCollection->_collection->_collection;
           TRI_ASSERT(document != nullptr);
         
           if (trxCollection->_barrier == nullptr) {  
             trxCollection->_barrier = TRI_CreateBarrierElement(&document->_barrierList);
           }

           if (trxCollection->_barrier != nullptr) {
              // tell everyone else this barrier is still in use,
              // at least until the transaction is over
             reinterpret_cast<TRI_barrier_blocker_t*>(trxCollection->_barrier)->_usedByTransaction = true;
           }

           return trxCollection->_barrier;
         }

// -----------------------------------------------------------------------------
// --SECTION--                                                 protected methods
// -----------------------------------------------------------------------------

      protected:

////////////////////////////////////////////////////////////////////////////////
/// @brief return the collection
////////////////////////////////////////////////////////////////////////////////
         
         TRI_document_collection_t* documentCollection (TRI_transaction_collection_t const* trxCollection) const {
           TRI_ASSERT(_trx != nullptr);
           TRI_ASSERT(getStatus() == TRI_TRANSACTION_RUNNING);
           TRI_ASSERT(trxCollection->_collection != nullptr);
           TRI_ASSERT(trxCollection->_collection->_collection != nullptr);

           return trxCollection->_collection->_collection;
         }

////////////////////////////////////////////////////////////////////////////////
/// @brief return a collection's shaper
////////////////////////////////////////////////////////////////////////////////
         
         TRI_shaper_t* shaper (TRI_transaction_collection_t const* trxCollection) const {
           TRI_ASSERT(_trx != nullptr);
           TRI_ASSERT(getStatus() == TRI_TRANSACTION_RUNNING);
           TRI_ASSERT(trxCollection->_collection != nullptr);
           TRI_ASSERT(trxCollection->_collection->_collection != nullptr);

           return trxCollection->_collection->_collection->_shaper;
         }

////////////////////////////////////////////////////////////////////////////////
/// @brief add a collection by id, with the name supplied
////////////////////////////////////////////////////////////////////////////////
        
        int addCollection (TRI_voc_cid_t cid,
                           char const* name,
                           TRI_transaction_type_e type) {
          int res = this->addCollection(cid, type);

          if (res != TRI_ERROR_NO_ERROR) {
            _errorData = string(name);
          }

          return res;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief add a collection by id
////////////////////////////////////////////////////////////////////////////////

        int addCollection (TRI_voc_cid_t cid,
                           TRI_transaction_type_e type) {
          if (_trx == nullptr) {
            return registerError(TRI_ERROR_INTERNAL);
          }
         
          if (cid == 0) {
            // invalid cid
            return registerError(TRI_ERROR_ARANGO_COLLECTION_NOT_FOUND);
          }

          const TRI_transaction_status_e status = getStatus();
              
          if (status == TRI_TRANSACTION_COMMITTED ||
              status == TRI_TRANSACTION_ABORTED) {
            // transaction already finished?
            return registerError(TRI_ERROR_TRANSACTION_INTERNAL);
          }

          int res;

          if (this->isEmbeddedTransaction()) {
            res = this->addCollectionEmbedded(cid, type);
          }
          else {
            res = this->addCollectionToplevel(cid, type);
          }
          
          return res;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief add a collection by name
////////////////////////////////////////////////////////////////////////////////

        int addCollection (std::string const& name,
                           TRI_transaction_type_e type) {
          if (! _isReal) {
            return addCollection(this->resolver()->getCollectionIdCluster(name), name.c_str(), type);
          }

          return addCollection(this->resolver()->getCollectionId(name), name.c_str(), type);
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief set the lock acquisition timeout
////////////////////////////////////////////////////////////////////////////////

        void setTimeout (double timeout) {
          _timeout = timeout; 
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief set the waitForSync property
////////////////////////////////////////////////////////////////////////////////

        void setWaitForSync () {
          _waitForSync = true;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief add a transaction hint
////////////////////////////////////////////////////////////////////////////////

        void addHint (TRI_transaction_hint_e hint) {
          _hints |= (TRI_transaction_hint_t) hint;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief read- or write-lock a collection
////////////////////////////////////////////////////////////////////////////////

        int lock (TRI_transaction_collection_t* trxCollection,
                  TRI_transaction_type_e type) {

          if (_trx == nullptr || getStatus() != TRI_TRANSACTION_RUNNING) {
            return TRI_ERROR_TRANSACTION_INTERNAL;
          }

          int res = TRI_LockCollectionTransaction(trxCollection, type, _nestingLevel);

          return res;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief read- or write-unlock a collection
////////////////////////////////////////////////////////////////////////////////

        int unlock (TRI_transaction_collection_t* trxCollection,
                    TRI_transaction_type_e type) {

          if (_trx == nullptr || getStatus() != TRI_TRANSACTION_RUNNING) {
            return TRI_ERROR_TRANSACTION_INTERNAL;
          }

          int res = TRI_UnlockCollectionTransaction(trxCollection, type, _nestingLevel);

          return res;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief read- or write-unlock a collection
////////////////////////////////////////////////////////////////////////////////

        bool isLocked (TRI_transaction_collection_t* const trxCollection,
                       TRI_transaction_type_e type) {
          if (_trx == nullptr || getStatus() != TRI_TRANSACTION_RUNNING) {
            return false;
          }

          bool locked = TRI_IsLockedCollectionTransaction(trxCollection, type, _nestingLevel);

          return locked;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief read any (random) document
////////////////////////////////////////////////////////////////////////////////

        int readAny (TRI_transaction_collection_t* trxCollection,
                     TRI_doc_mptr_copy_t* mptr) {

          TRI_document_collection_t* document = documentCollection(trxCollection);

          // READ-LOCK START
          int res = this->lock(trxCollection, TRI_TRANSACTION_READ);

          if (res != TRI_ERROR_NO_ERROR) {
            return res;
          }

          if (document->_primaryIndex._nrUsed == 0) {
            // no document found
            mptr->setDataPtr(nullptr);  // PROTECTED by trx in trxCollection
          }
          else {
            if (orderBarrier(trxCollection) == nullptr) {
              return TRI_ERROR_OUT_OF_MEMORY;
            }

            size_t total = document->_primaryIndex._nrAlloc;
            size_t pos = TRI_UInt32Random() % total;
            void** beg = document->_primaryIndex._table;

            while (beg[pos] == 0) {
              pos = TRI_UInt32Random() % total;
            }

            *mptr = *((TRI_doc_mptr_t*) beg[pos]);
          }

          this->unlock(trxCollection, TRI_TRANSACTION_READ);
          // READ-LOCK END

          return TRI_ERROR_NO_ERROR;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief read a single document, identified by key
////////////////////////////////////////////////////////////////////////////////

        int readSingle (TRI_transaction_collection_t* trxCollection,
                        TRI_doc_mptr_copy_t* mptr,
                        std::string const& key) {

          TRI_ASSERT(mptr != nullptr);
          
          if (orderBarrier(trxCollection) == nullptr) {
            return TRI_ERROR_OUT_OF_MEMORY;
          }

          TRI_document_collection_t* document = documentCollection(trxCollection);

          int res = document->readDocument(trxCollection,
                                          (TRI_voc_key_t) key.c_str(), 
                                          mptr,
                                          ! isLocked(trxCollection, TRI_TRANSACTION_READ));

          return res;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief read all documents
////////////////////////////////////////////////////////////////////////////////

        int readAll (TRI_transaction_collection_t* trxCollection,
                     std::vector<std::string>& ids, 
                     bool lock) {

          TRI_document_collection_t* document = documentCollection(trxCollection);

          if (lock) {
            // READ-LOCK START
            int res = this->lock(trxCollection, TRI_TRANSACTION_READ);
          
            if (res != TRI_ERROR_NO_ERROR) {
              return res;
            }
          }
    
          if (document->_primaryIndex._nrUsed > 0) {
            if (orderBarrier(trxCollection) == nullptr) {
              return TRI_ERROR_OUT_OF_MEMORY;
            }

            ids.reserve(document->_primaryIndex._nrUsed);

            void** ptr = document->_primaryIndex._table;
            void** end = ptr + document->_primaryIndex._nrAlloc;

            for (;  ptr < end;  ++ptr) {
              if (*ptr) {
                TRI_doc_mptr_t const* d = (TRI_doc_mptr_t const*) *ptr;
                ids.push_back(TRI_EXTRACT_MARKER_KEY(d));  // PROTECTED by trx in trxCollection
              }
            }
          }
          
          if (lock) {
            this->unlock(trxCollection, TRI_TRANSACTION_READ);
            // READ-LOCK END
          }

          return TRI_ERROR_NO_ERROR;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief read master pointers in order of insertion/update
////////////////////////////////////////////////////////////////////////////////

        int readOrdered (TRI_transaction_collection_t* trxCollection, 
                         std::vector<TRI_doc_mptr_copy_t>& documents,
                         int64_t offset,
                         int64_t count) {
          TRI_document_collection_t* document = documentCollection(trxCollection);

          // READ-LOCK START
          int res = this->lock(trxCollection, TRI_TRANSACTION_READ);

          if (res != TRI_ERROR_NO_ERROR) {
            return res;
          }

          if (orderBarrier(trxCollection) == nullptr) {
            return TRI_ERROR_OUT_OF_MEMORY;
          }

          TRI_doc_mptr_t* doc;

          if (offset >= 0) {
            // read from front
            doc = document->_headersPtr->front(document->_headersPtr);  // PROTECTED by trx in trxCollection
            int64_t i = 0;

            while (doc != nullptr && i < offset) {
              doc = doc->_next;
              ++i;
            }

            i = 0;
            while (doc != nullptr && i < count) {
              documents.emplace_back(*doc);
              doc = doc->_next;
              ++i;
            }
          }
          else {
            // read from back
            doc = document->_headersPtr->back(document->_headersPtr);  // PROTECTED by trx in trxCollection
            int64_t i = -1;

            while (doc != nullptr && i > offset) {
              doc = doc->_prev;
              --i;
            }

            i = 0;
            while (doc != nullptr && i < count) {
              documents.emplace_back(*doc);
              doc = doc->_prev;
              ++i;
            }
          }

          this->unlock(trxCollection, TRI_TRANSACTION_READ);
          // READ-LOCK END

          return TRI_ERROR_NO_ERROR;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief read all master pointers, using skip and limit
////////////////////////////////////////////////////////////////////////////////

        int readSlice (TRI_transaction_collection_t* trxCollection,
                       std::vector<TRI_doc_mptr_copy_t>& docs,
                       TRI_voc_ssize_t skip,
                       TRI_voc_size_t limit,
                       uint32_t* total) {
          
          TRI_document_collection_t* document = documentCollection(trxCollection);

          if (limit == 0) {
            // nothing to do
            return TRI_ERROR_NO_ERROR;
          }

          // READ-LOCK START
          int res = this->lock(trxCollection, TRI_TRANSACTION_READ);

          if (res != TRI_ERROR_NO_ERROR) {
            return res;
          }
          
          if (document->_primaryIndex._nrUsed == 0) {
            // nothing to do
            this->unlock(trxCollection, TRI_TRANSACTION_READ);
            // READ-LOCK END
            return TRI_ERROR_NO_ERROR;
          }

          if (orderBarrier(trxCollection) == nullptr) {
            return TRI_ERROR_OUT_OF_MEMORY;
          }

          void** beg = document->_primaryIndex._table;
          void** ptr = beg;
          void** end = ptr + document->_primaryIndex._nrAlloc;
          uint32_t count = 0;
          
          *total = (uint32_t) document->_primaryIndex._nrUsed;

          // apply skip
          if (skip > 0) {
            // skip from the beginning
            for (;  ptr < end && 0 < skip;  ++ptr) {
              if (*ptr) {
                --skip;
              }
            }
          }
          else if (skip < 0) {
            // skip from the end
            ptr = end - 1;

            for (; beg <= ptr; --ptr) {
              if (*ptr) {
                ++skip;

                if (skip == 0) {
                  break;
                }
              }
            }

            if (ptr < beg) {
              ptr = beg;
            }
          }

          // fetch documents, taking limit into account
          for (; ptr < end && count < limit; ++ptr) {
            if (*ptr) {
              TRI_doc_mptr_t* d = (TRI_doc_mptr_t*) *ptr;

              docs.emplace_back(*d);
              ++count;
            }
          }

          this->unlock(trxCollection, TRI_TRANSACTION_READ);
          // READ-LOCK END

          return TRI_ERROR_NO_ERROR;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief read all master pointers, using skip and limit and an internal
/// offset into the primary index. this can be used for incremental access to
/// the documents without restarting the index scan at the begin
////////////////////////////////////////////////////////////////////////////////

        int readIncremental (TRI_transaction_collection_t* trxCollection,
                             std::vector<TRI_doc_mptr_copy_t>& docs,
                             TRI_voc_size_t& internalSkip,
                             TRI_voc_size_t batchSize,
                             TRI_voc_ssize_t skip,
                             uint32_t* total) {
          
          TRI_document_collection_t* document = documentCollection(trxCollection);

          // READ-LOCK START
          int res = this->lock(trxCollection, TRI_TRANSACTION_READ);

          if (res != TRI_ERROR_NO_ERROR) {
            return res;
          }
            
          if (document->_primaryIndex._nrUsed == 0) {
            // nothing to do
            this->unlock(trxCollection, TRI_TRANSACTION_READ);

            // READ-LOCK END
            return TRI_ERROR_NO_ERROR;
          }

          if (orderBarrier(trxCollection) == nullptr) {
            return TRI_ERROR_OUT_OF_MEMORY;
          }

          void** beg = document->_primaryIndex._table;
          void** end = beg + document->_primaryIndex._nrAlloc;

          if (internalSkip > 0) {
            beg += internalSkip;
          }

          void** ptr = beg;
          uint32_t count = 0;
          *total = (uint32_t) document->_primaryIndex._nrUsed;

          // fetch documents, taking limit into account
          for (; ptr < end && count < batchSize; ++ptr, ++internalSkip) {
            if (*ptr) {
              TRI_doc_mptr_t* d = (TRI_doc_mptr_t*) *ptr;

              if (skip > 0) {
                --skip;
              }
              else {
                docs.emplace_back(*d);
                ++count;
              }
            }
          }
            
          this->unlock(trxCollection, TRI_TRANSACTION_READ);
          // READ-LOCK END

          return TRI_ERROR_NO_ERROR;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief create a single document, using JSON
////////////////////////////////////////////////////////////////////////////////

        int create (TRI_transaction_collection_t* trxCollection,
                    TRI_df_marker_type_e markerType,
                    TRI_doc_mptr_copy_t* mptr,
                    TRI_json_t const* json,
                    void const* data,
                    bool forceSync) {

          TRI_voc_key_t key = 0;
          int res = DocumentHelper::getKey(json, &key);

          if (res != TRI_ERROR_NO_ERROR) {
            return res;
          }

          TRI_shaper_t* shaper = this->shaper(trxCollection);
          TRI_memory_zone_t* zone = shaper->_memoryZone;
          TRI_shaped_json_t* shaped = TRI_ShapedJsonJson(shaper, json, true, isLocked(trxCollection, TRI_TRANSACTION_WRITE));

          if (shaped == nullptr) {
            return TRI_ERROR_ARANGO_SHAPER_FAILED;
          }

          res = create(trxCollection, 
                       key, 
                       0,
                       markerType, 
                       mptr, 
                       shaped, 
                       data, 
                       forceSync); 

          TRI_FreeShapedJson(zone, shaped);

          return res;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief create a single document, using shaped json
////////////////////////////////////////////////////////////////////////////////

        inline int create (TRI_transaction_collection_t* trxCollection,
                           const TRI_voc_key_t key,
                           TRI_voc_rid_t rid,
                           const TRI_df_marker_type_e markerType,
                           TRI_doc_mptr_copy_t* mptr,
                           TRI_shaped_json_t const* shaped,
                           void const* data,
                           bool forceSync) {
         
          TRI_document_collection_t* document = documentCollection(trxCollection);
          bool lock = ! isLocked(trxCollection, TRI_TRANSACTION_WRITE); 

          int res = document->insertDocument(trxCollection,
                                            key, 
                                            rid, 
                                            mptr,
                                            markerType,
                                            shaped,
                                            static_cast<TRI_document_edge_t const*>(data),
                                            lock,
                                            forceSync,
                                            false);

          return res;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief update a single document, using JSON
////////////////////////////////////////////////////////////////////////////////

        int update (TRI_transaction_collection_t* trxCollection,
                    std::string const& key,
                    TRI_voc_rid_t rid,
                    TRI_doc_mptr_copy_t* mptr,
                    TRI_json_t* const json,
                    TRI_doc_update_policy_e policy,
                    TRI_voc_rid_t expectedRevision,
                    TRI_voc_rid_t* actualRevision,
                    bool forceSync) {

          TRI_shaper_t* shaper = this->shaper(trxCollection);
          TRI_memory_zone_t* zone = shaper->_memoryZone;
          TRI_shaped_json_t* shaped = TRI_ShapedJsonJson(shaper, json, true, isLocked(trxCollection, TRI_TRANSACTION_WRITE));

          if (shaped == nullptr) {
            return TRI_ERROR_ARANGO_SHAPER_FAILED;
          }
          
          if (orderBarrier(trxCollection) == nullptr) {
            return TRI_ERROR_OUT_OF_MEMORY;
          }

          int res = update(trxCollection, 
                           key, 
                           rid,
                           mptr, 
                           shaped, 
                           policy, 
                           expectedRevision, 
                           actualRevision, 
                           forceSync); 

          TRI_FreeShapedJson(zone, shaped);

          return res;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief update a single document, using shaped json
////////////////////////////////////////////////////////////////////////////////

        inline int update (TRI_transaction_collection_t* const trxCollection,
                           std::string const& key,
                           TRI_voc_rid_t rid,
                           TRI_doc_mptr_copy_t* mptr,
                           TRI_shaped_json_t* const shaped,
                           TRI_doc_update_policy_e policy,
                           TRI_voc_rid_t expectedRevision,
                           TRI_voc_rid_t* actualRevision,
                           bool forceSync) {

          TRI_doc_update_policy_t updatePolicy(policy, expectedRevision, actualRevision);
          
          if (orderBarrier(trxCollection) == nullptr) {
            return TRI_ERROR_OUT_OF_MEMORY;
          }

          TRI_document_collection_t* document = documentCollection(trxCollection);
          
          int res = document->updateDocument(trxCollection, 
                                            (TRI_voc_key_t) key.c_str(),
                                            rid, 
                                            mptr, 
                                            shaped, 
                                            &updatePolicy, 
                                            ! isLocked(trxCollection, TRI_TRANSACTION_WRITE), 
                                            forceSync);
          
          return res;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief delete a single document
////////////////////////////////////////////////////////////////////////////////

        int remove (TRI_transaction_collection_t* trxCollection,
                    std::string const& key,
                    TRI_voc_rid_t rid,
                    TRI_doc_update_policy_e policy,
                    TRI_voc_rid_t expectedRevision,
                    TRI_voc_rid_t* actualRevision,
                    bool forceSync) {
          
          TRI_doc_update_policy_t updatePolicy(policy, expectedRevision, actualRevision);
          
          TRI_document_collection_t* document = documentCollection(trxCollection);

          int res = document->removeDocument(trxCollection,
                                            (TRI_voc_key_t) key.c_str(), 
                                            rid,
                                            &updatePolicy, 
                                            ! isLocked(trxCollection, TRI_TRANSACTION_WRITE), 
                                            forceSync);

          return res;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief truncate a collection
/// the caller must make sure a barrier is held
////////////////////////////////////////////////////////////////////////////////

        int removeAll (TRI_transaction_collection_t* const trxCollection,
                       bool forceSync) {

          std::vector<std::string> ids;
          
          TRI_document_collection_t* document = documentCollection(trxCollection);
          
          if (orderBarrier(trxCollection) == nullptr) {
            return TRI_ERROR_OUT_OF_MEMORY;
          }
          
          // WRITE-LOCK START
          int res = this->lock(trxCollection, TRI_TRANSACTION_WRITE);
          
          if (res != TRI_ERROR_NO_ERROR) {
            return res;
          }
          
          res = readAll(trxCollection, ids, false);

          if (res != TRI_ERROR_NO_ERROR) {
            this->unlock(trxCollection, TRI_TRANSACTION_WRITE);
            return res;
          }

          for (auto it = ids.begin(); it != ids.end(); ++it) {
            res = document->removeDocument(trxCollection,
                                          (TRI_voc_key_t) (*it).c_str(), 
                                          0,
                                          nullptr, // policy
                                          false,
                                          forceSync);


            if (res != TRI_ERROR_NO_ERROR) {
              // halt on first error
              break;
            }
          }

          this->unlock(trxCollection, TRI_TRANSACTION_WRITE);
          // WRITE-LOCK END

          return res;
        }

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief register an error for the transaction
////////////////////////////////////////////////////////////////////////////////

        int registerError (int errorNum) {
          TRI_ASSERT(errorNum != TRI_ERROR_NO_ERROR);

          if (_setupState == TRI_ERROR_NO_ERROR) {
            _setupState = errorNum;
          }

          TRI_ASSERT(_setupState != TRI_ERROR_NO_ERROR);

          return errorNum;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief add a collection to an embedded transaction
////////////////////////////////////////////////////////////////////////////////

        int addCollectionEmbedded (TRI_voc_cid_t cid, 
                                   TRI_transaction_type_e type) {
          TRI_ASSERT(_trx != nullptr);

          int res = TRI_AddCollectionTransaction(_trx, cid, type, _nestingLevel);

          if (res != TRI_ERROR_NO_ERROR) {
            return registerError(res);
          }

          return TRI_ERROR_NO_ERROR;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief add a collection to a top-level transaction
////////////////////////////////////////////////////////////////////////////////

        int addCollectionToplevel (TRI_voc_cid_t cid, 
                                   TRI_transaction_type_e type) {
          TRI_ASSERT(_trx != nullptr);
          
          int res;

          if (getStatus() != TRI_TRANSACTION_CREATED) {
            // transaction already started?
            res = TRI_ERROR_TRANSACTION_INTERNAL;
          }
          else {
            res = TRI_AddCollectionTransaction(_trx, cid, type, _nestingLevel);
          }

          if (res != TRI_ERROR_NO_ERROR) {
            registerError(res);
          }

          return res;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief initialise the transaction
/// this will first check if the transaction is embedded in a parent 
/// transaction. if not, it will create a transaction of its own
////////////////////////////////////////////////////////////////////////////////

        int setupTransaction () {
          // check in the context if we are running embedded
          _trx = this->getParentTransaction();

          if (_trx != nullptr) {
            // yes, we are embedded
            _setupState = setupEmbedded();
          }
          else {
            // non-embedded
            _setupState = setupToplevel();
          }

          // this may well be TRI_ERROR_NO_ERROR...
          return _setupState;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief set up an embedded transaction
////////////////////////////////////////////////////////////////////////////////

        int setupEmbedded () {
          TRI_ASSERT(_nestingLevel == 0);

          _nestingLevel = ++_trx->_nestingLevel;
            
          if (! this->isEmbeddable()) {
            // we are embedded but this is disallowed...
            LOG_WARNING("logic error. invalid nesting of transactions");

            return TRI_ERROR_TRANSACTION_NESTED;
          }
            
          return TRI_ERROR_NO_ERROR;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief set up a top-level transaction
////////////////////////////////////////////////////////////////////////////////

        int setupToplevel () {
          TRI_ASSERT(_nestingLevel == 0);

          // we are not embedded. now start our own transaction 
          _trx = TRI_CreateTransaction(_vocbase,
                                       _generatingServer,
                                       _replicate, 
                                       _timeout, 
                                       _waitForSync);

          if (_trx == nullptr) {
            return TRI_ERROR_OUT_OF_MEMORY;
          }

          // register the transaction in the context
          return this->registerTransaction(_trx);
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief free transaction
////////////////////////////////////////////////////////////////////////////////

        int freeTransaction () {
          TRI_ASSERT(! isEmbeddedTransaction());

          if (_trx != nullptr) {
            this->unregisterTransaction();

            TRI_FreeTransaction(_trx);
            _trx = nullptr;
          }

          return TRI_ERROR_NO_ERROR;
        }

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief error that occurred on transaction initialisation (before begin())
////////////////////////////////////////////////////////////////////////////////

        int _setupState;

////////////////////////////////////////////////////////////////////////////////
/// @brief how deep the transaction is down in a nested transaction structure
////////////////////////////////////////////////////////////////////////////////

        int _nestingLevel;

////////////////////////////////////////////////////////////////////////////////
/// @brief additional error data
////////////////////////////////////////////////////////////////////////////////

        std::string _errorData;

////////////////////////////////////////////////////////////////////////////////
/// @brief transaction hints
////////////////////////////////////////////////////////////////////////////////

        TRI_transaction_hint_t _hints;

////////////////////////////////////////////////////////////////////////////////
/// @brief timeout for lock acquisition
////////////////////////////////////////////////////////////////////////////////

        double _timeout;

////////////////////////////////////////////////////////////////////////////////
/// @brief wait for sync property for transaction
////////////////////////////////////////////////////////////////////////////////

        bool _waitForSync;

////////////////////////////////////////////////////////////////////////////////
/// @brief replicate flag for transaction
////////////////////////////////////////////////////////////////////////////////

        bool _replicate;

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not this is a "real" transaction
////////////////////////////////////////////////////////////////////////////////

        bool _isReal;

// -----------------------------------------------------------------------------
// --SECTION--                                               protected variables
// -----------------------------------------------------------------------------

      protected:

////////////////////////////////////////////////////////////////////////////////
/// @brief the C transaction struct
////////////////////////////////////////////////////////////////////////////////

        TRI_transaction_t* _trx;

////////////////////////////////////////////////////////////////////////////////
/// @brief the vocbase
////////////////////////////////////////////////////////////////////////////////

        TRI_vocbase_t* const _vocbase;

////////////////////////////////////////////////////////////////////////////////
/// @brief the id of the generating server
////////////////////////////////////////////////////////////////////////////////

        TRI_server_id_t _generatingServer;

    };

  }
}

#endif

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @addtogroup\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
