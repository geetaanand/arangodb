////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
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
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_IRESEARCH__IRESEARCH_VIEW_COORDINATOR_H
#define ARANGODB_IRESEARCH__IRESEARCH_VIEW_COORDINATOR_H 1

#include "VocBase/LogicalView.h"
#include "IResearch/IResearchViewMeta.h"

#include <velocypack/Builder.h>
#include <velocypack/Slice.h>

namespace arangodb {

struct ViewFactory; // forward declaration

} // arangodb

namespace arangodb {
namespace iresearch {

///////////////////////////////////////////////////////////////////////////////
/// @class IResearchViewCoordinator
/// @brief an abstraction over the distributed IResearch index implementing the
///        LogicalView interface
///////////////////////////////////////////////////////////////////////////////
class IResearchViewCoordinator final : public arangodb::LogicalViewClusterInfo {
 public:

  using LogicalView::drop;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief remove all documents matching collection 'cid' from this IResearch
  ///        View
  //////////////////////////////////////////////////////////////////////////////
  arangodb::Result drop(TRI_voc_cid_t) noexcept { return arangodb::Result(); } // NOOP since no internal store

  ////////////////////////////////////////////////////////////////////////////////
  /// @brief acquire locks on the specified 'cid' during read-transactions
  ///        allowing retrieval of documents contained in the aforementioned
  ///        collection
  /// @note definitions are not persisted
  /// @param cid the collection ID to track
  /// @param key the key of the link definition for use in appendVelocyPack(...)
  /// @param value the link definition to use in appendVelocyPack(...)
  /// @return the 'cid' was newly added to the IResearch View
  ////////////////////////////////////////////////////////////////////////////////
  bool emplace(
    TRI_voc_cid_t cid,
    std::string const& key,
    arangodb::velocypack::Slice const& value
  );

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the factory for this type of view
  //////////////////////////////////////////////////////////////////////////////
  static arangodb::ViewFactory const& factory();

  bool visitCollections(CollectionVisitor const& visitor) const override;

  void open() override {
    // NOOP
  }

  virtual Result rename(
      std::string&& /*newName*/,
      bool /*doSync*/
  ) override {
    // not supported in a cluster
    return { TRI_ERROR_NOT_IMPLEMENTED };
  }

  virtual arangodb::Result updateProperties(
    velocypack::Slice const& properties,
    bool partialUpdate,
    bool doSync
  ) override;

 protected:
  virtual Result appendVelocyPackDetailed(
      arangodb::velocypack::Builder& builder,
      bool forPersistence
  ) const override;

  virtual arangodb::Result dropImpl() override;

 private:
  struct ViewFactory; // forward declaration

  IResearchViewCoordinator(
    TRI_vocbase_t& vocbase, velocypack::Slice info, uint64_t planVersion
  );

  std::unordered_map<TRI_voc_cid_t, std::pair<std::string, arangodb::velocypack::Builder>> _collections; // transient member, not persisted
  mutable irs::async_utils::read_write_mutex _mutex; // for use with '_collections'
  IResearchViewMeta _meta;
}; // IResearchViewCoordinator

} // iresearch
} // arangodb

#endif // ARANGODB_IRESEARCH__IRESEARCH_VIEW_COORDINATOR_H
