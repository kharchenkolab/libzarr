// SPDX-License-Identifier: MIT

#ifndef LIBZARR_GROUP_HPP
#define LIBZARR_GROUP_HPP

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "libzarr/array.hpp"
#include "libzarr/metadata.hpp"
#include "libzarr/store.hpp"
#include "libzarr/types.hpp"
#include "libzarr/v2.hpp"

/// \file group.hpp
/// The Group API: hierarchy create/open/traverse. Groups are value-semantics
/// handles sharing the store; opening the root through consolidated metadata
/// (.zmetadata) is automatic when present, so remote stores pay one metadata
/// round-trip.

namespace zarr {

/// Immediate children of a group, by kind.
struct GroupChildren {
  /// Names of child arrays, sorted.
  std::vector<std::string> arrays;
  /// Names of child groups, sorted.
  std::vector<std::string> groups;
};

/// A Zarr group bound to a Store.
class Group {
 public:
  /// Creates a v2 group at `path` ("" = store root), including any missing
  /// ancestor groups (writers that skip intermediate .zgroup documents are a
  /// known interop hazard).
  static Group create(std::shared_ptr<Store> store, const std::string& path = "",
                      ZarrFormat format = ZarrFormat::v2) {
    if (!store) {
      throw error("Group::create: null store");
    }
    if (format != ZarrFormat::v2) {
      throw error("Group::create: v3 write support arrives in a later phase");
    }
    detail::validate_path(path);
    write_group_chain(*store, path);
    return {std::move(store), path, json::object(), nullptr};
  }

  /// Opens the group at `path`. At the root, consolidated metadata is used
  /// when present and shared with every node opened through this group.
  static Group open(std::shared_ptr<Store> store, const std::string& path = "") {
    if (!store) {
      throw error("Group::open: null store");
    }
    detail::validate_path(path);
    std::shared_ptr<const json> consolidated;
    if (const auto c = v2::read_consolidated(*store)) {
      consolidated = std::make_shared<const json>(*c);
    }
    return open_with(std::move(store), path, std::move(consolidated));
  }

  /// Node path within the store ("" = root).
  [[nodiscard]] const std::string& path() const { return path_; }

  /// User attributes (.zattrs).
  [[nodiscard]] const json& attributes() const { return attributes_; }

  /// Replaces the user attributes and persists them.
  void set_attributes(json attributes) {
    attributes_ = std::move(attributes);
    const std::string key = v2::meta_key(path_, v2::kAttrsSuffix);
    if (attributes_.empty()) {
      v2::erase_meta_key(*store_, key);  // canonical: no empty .zattrs documents
    } else {
      v2::write_meta_key(*store_, key, attributes_);
    }
  }

  /// Creates a child (possibly nested, e.g. "a/b") group.
  Group create_group(const std::string& name) {
    return create(store_, child_path(name), ZarrFormat::v2);
  }

  /// Creates a child (possibly nested) array, writing ancestor groups first.
  Array create_array(const std::string& name, const ArraySpec& spec) {
    const std::string target = child_path(name);
    const std::size_t slash = target.rfind('/');
    if (slash != std::string::npos) {
      write_group_chain(*store_, target.substr(0, slash));
    }
    return Array::create(store_, target, spec);
  }

  /// Opens a child (possibly nested) group.
  [[nodiscard]] Group open_group(const std::string& name) const {
    return open_with(store_, child_path(name), consolidated_);
  }

  /// Opens a child (possibly nested) array.
  [[nodiscard]] Array open_array(const std::string& name) const {
    return Array::open(store_, child_path(name), consolidated_);
  }

  /// Lists immediate children, classified by their metadata documents.
  [[nodiscard]] GroupChildren children() const {
    GroupChildren out;
    const std::string prefix = path_.empty() ? "" : path_ + "/";
    for (const std::string& child : store_->list_dir(prefix).prefixes) {
      const std::string child_full = prefix + child;
      if (store_->exists(child_full + "/" + v2::kArraySuffix)) {
        out.arrays.push_back(child);
      } else if (store_->exists(child_full + "/" + v2::kGroupSuffix)) {
        out.groups.push_back(child);
      }
      // Other directories are not Zarr nodes; ignore them.
    }
    return out;
  }

 private:
  Group(std::shared_ptr<Store> store, std::string path, json attributes,
        std::shared_ptr<const json> consolidated)
      : store_(std::move(store)),
        path_(std::move(path)),
        attributes_(std::move(attributes)),
        consolidated_(std::move(consolidated)) {}

  static Group open_with(std::shared_ptr<Store> store, const std::string& path,
                         std::shared_ptr<const json> consolidated) {
    const auto read_doc = [&](const std::string& key) -> std::optional<json> {
      if (consolidated) {
        const auto it = consolidated->find(key);
        if (it == consolidated->end()) {
          return std::nullopt;
        }
        return *it;
      }
      const auto bytes = store->read(key);
      if (!bytes) {
        return std::nullopt;
      }
      return v2::parse_json(*bytes, key);
    };

    const std::string group_key = v2::meta_key(path, v2::kGroupSuffix);
    const auto doc = read_doc(group_key);
    if (!doc) {
      if (store->exists(v2::meta_key(path, "zarr.json"))) {
        throw error("'" + path + "' is a Zarr v3 node; v3 support arrives in a later phase");
      }
      if (store->exists(v2::meta_key(path, v2::kArraySuffix))) {
        throw error("'" + path + "' is an array, not a group");
      }
      throw error("no group at '" + path + "' (" + group_key + " not found)");
    }
    v2::check_group_meta(*doc, group_key);
    json attributes = json::object();
    if (const auto attrs = read_doc(v2::meta_key(path, v2::kAttrsSuffix))) {
      attributes = *attrs;
    }
    return {std::move(store), path, std::move(attributes), std::move(consolidated)};
  }

  /// Writes .zgroup at `path` and every missing ancestor.
  static void write_group_chain(Store& store, const std::string& path) {
    std::vector<std::string> chain;
    chain.emplace_back("");
    std::size_t start = 0;
    while (!path.empty()) {
      const std::size_t slash = path.find('/', start);
      if (slash == std::string::npos) {
        chain.push_back(path);
        break;
      }
      chain.push_back(path.substr(0, slash));
      start = slash + 1;
    }
    for (const std::string& node : chain) {
      const std::string key = v2::meta_key(node, v2::kGroupSuffix);
      if (store.exists(v2::meta_key(node, v2::kArraySuffix))) {
        throw error("'" + node + "' is an array; cannot create a group inside it");
      }
      if (!store.exists(key)) {
        v2::write_meta_key(store, key, v2::group_meta_json());
      }
    }
  }

  [[nodiscard]] std::string child_path(const std::string& name) const {
    detail::validate_path(name);
    if (name.empty()) {
      throw error("child name must not be empty");
    }
    return path_.empty() ? name : path_ + "/" + name;
  }

  std::shared_ptr<Store> store_;
  std::string path_;
  json attributes_;
  std::shared_ptr<const json> consolidated_;
};

}  // namespace zarr

#endif  // LIBZARR_GROUP_HPP
