/*
 * Copyright 2016-2023 ClickHouse, Inc.
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


/*
 * This file may have been modified by Bytedance Ltd. and/or its affiliates (“ Bytedance's Modifications”).
 * All Bytedance's Modifications are Copyright (2023) Bytedance Ltd. and/or its affiliates.
 */

#pragma once

#include <Compression/CompressionFactory.h>
#include <Core/Block.h>
#include <Core/Names.h>
#include <Core/NamesAndTypes.h>
#include <Core/NamesAndAliases.h>
#include <Interpreters/Context_fwd.h>
#include <Storages/ColumnCodec.h>
#include <Storages/ColumnDefault.h>
#include <Common/Exception.h>

#include <boost/multi_index/member.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <optional>


namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

struct GetColumnsOptions
{
    enum Kind : UInt8
    {
        Ordinary = 1,
        Materialized = 2,
        Aliases = 4,
        Ephemeral = 8,

        AllPhysical = Ordinary | Materialized,
        All = AllPhysical | Aliases | Ephemeral,
    };

    GetColumnsOptions(Kind kind_) : kind(kind_) {}

    GetColumnsOptions & withSubcolumns(bool value = true)
    {
        with_subcolumns = value;
        return *this;
    }

    GetColumnsOptions & withVirtuals(bool value = true)
    {
        with_virtuals = value;
        return *this;
    }

    GetColumnsOptions & withExtendedObjects(bool value = true)
    {
        with_extended_objects = value;
        return *this;
    }

        GetColumnsOptions & withSystemColumns(bool value = true)
    {
        with_system_columns = value;
        return *this;
    }

    Kind kind;
    bool with_subcolumns = false;
    bool with_virtuals = false;
    bool with_extended_objects = false;
    bool with_system_columns = false;
};

/// Description of a single table column (in CREATE TABLE for example).
struct ColumnDescription
{
    String name;
    DataTypePtr type;
    ColumnDefault default_desc;
    /// TODO: Wrap on update expression using a structure similar to default_desc
    ASTPtr on_update_expression;
    /// For partial update, this means the imported data will only be replaced when it is of non-null value
    bool replace_if_not_null = false;
    String comment;
    ASTPtr codec;
    ASTPtr ttl;

    ColumnDescription() = default;
    ColumnDescription(ColumnDescription &&) = default;
    ColumnDescription(const ColumnDescription &) = default;
    ColumnDescription(String name_, DataTypePtr type_);

    bool operator==(const ColumnDescription & other) const;
    bool operator!=(const ColumnDescription & other) const { return !(*this == other); }

    void writeText(WriteBuffer & buf) const;
    void readText(ReadBuffer & buf);

    /// used for BitEngine to judge whethere the column want String as key type
    /// if column is not a bitengine column, the function is meanless
    bool hasBitEngineKeyStringInComment() const;
};


/// Description of multiple table columns (in CREATE TABLE for example).
class ColumnsDescription
{
public:
    ColumnsDescription() = default;
    explicit ColumnsDescription(NamesAndTypesList ordinary);

    explicit ColumnsDescription(NamesAndTypesList ordinary, NamesAndAliases aliases);

    /// `after_column` can be a Nested column name;
    void add(ColumnDescription column, const String & after_column = String(), bool first = false, bool add_subcolumns = true);
    /// `column_name` can be a Nested column name;
    void remove(const String & column_name);

    /// Rename column. column_from and column_to cannot be nested columns.
    /// TODO add ability to rename nested columns
    void rename(const String & column_from, const String & column_to);

    /// NOTE Must correspond with Nested::flatten function.
    void flattenNested(); /// TODO: remove, insert already flattened Nested columns.

    bool operator==(const ColumnsDescription & other) const { return columns == other.columns; }
    bool operator!=(const ColumnsDescription & other) const { return !(*this == other); }

    auto begin() const { return columns.begin(); }
    auto end() const { return columns.end(); }

    NamesAndTypesList get(const GetColumnsOptions & options) const;
    NamesAndTypesList getByNames(const GetColumnsOptions & options, const Names & names) const;

    NamesAndTypesList getOrdinary() const;
    NamesAndTypesList getMaterialized() const;
    NamesAndTypesList getAliases() const;
    NamesAndTypesList getAllPhysical() const; /// ordinary + materialized.
    NamesAndTypesList getAll() const; /// ordinary + materialized + aliases
    NamesAndTypesList getAllWithSubcolumns() const;
    NamesAndTypesList getAllPhysicalWithSubcolumns() const;
    NamesAndTypesList getSubcolumnsOfAllPhysical() const;
    NamesAndTypesList getSubcolumns(const String & name_in_storage) const;
    NamesAndTypesList getEphemeral() const;

    using ColumnTTLs = std::unordered_map<String, ASTPtr>;
    ColumnTTLs getColumnTTLs() const;

    bool has(const String & column_name) const;
    bool hasNested(const String & column_name) const;
    bool hasSubcolumn(const String & column_name) const;
    const ColumnDescription & get(const String & column_name) const;

    template <typename F>
    void modify(const String & column_name, F && f)
    {
        modify(column_name, String(), false, std::forward<F>(f));
    }

    template <typename F>
    void modify(const String & column_name, const String & after_column, bool first, F && f)
    {
        auto it = columns.get<1>().find(column_name);
        if (it == columns.get<1>().end())
            throw Exception("Cannot find column " + column_name + " in ColumnsDescription", ErrorCodes::LOGICAL_ERROR);

        removeSubcolumns(it->name);
        if (!columns.get<1>().modify(it, std::forward<F>(f)))
            throw Exception("Cannot modify ColumnDescription for column " + column_name + ": column name cannot be changed", ErrorCodes::LOGICAL_ERROR);

        addSubcolumns(it->name, it->type);
        modifyColumnOrder(column_name, after_column, first);
    }

    Names getNamesOfPhysical() const;
    Names getNamesOfOrdinary() const;
    Names getNamesOfOnUpdate() const;

    bool hasPhysical(const String & column_name) const;
    bool hasColumnOrSubcolumn(GetColumnsOptions::Kind kind, const String & column_name) const;

    NameAndTypePair getPhysical(const String & column_name) const;
    NameAndTypePair getColumnOrSubcolumn(GetColumnsOptions::Kind kind, const String & column_name) const;
    NameAndTypePair getColumn(const GetColumnsOptions & options, const String & column_name) const;

    std::optional<NameAndTypePair> tryGetPhysical(const String & column_name) const;
    std::optional<NameAndTypePair> tryGetColumnOrSubcolumn(GetColumnsOptions::Kind kind, const String & column_name) const;
    std::optional<NameAndTypePair> tryGetColumn(const GetColumnsOptions & options, const String & column_name) const;
    std::optional<NameAndTypePair> tryGetMapImplicitColumn(const String & column_name) const;

    std::optional<const ColumnDescription> tryGetColumnOrSubcolumnDescription(GetColumnsOptions::Kind kind, const String & column_name) const;
    std::optional<const ColumnDescription> tryGetColumnDescription(const GetColumnsOptions & options, const String & column_name) const;

    ColumnDefaults getDefaults() const; /// TODO: remove
    bool hasDefault(const String & column_name) const;
    bool hasDefaults() const;
    std::optional<ColumnDefault> getDefault(const String & column_name) const;
    using ColumnOnUpdates = std::unordered_map<std::string, ASTPtr>;
    ColumnOnUpdates getOnUpdates() const;
    NameSet getReplaceIfNotNullColumns() const;

    /// Does column has non default specified compression codec
    bool hasCompressionCodec(const String & column_name) const;
    CompressionCodecPtr getCodecOrDefault(const String & column_name, CompressionCodecPtr default_codec) const;
    CompressionCodecPtr getCodecOrDefault(const String & column_name) const;
    ASTPtr getCodecDescOrDefault(const String & column_name, CompressionCodecPtr default_codec) const;

    String toString() const;
    static ColumnsDescription parse(const String & str);

    size_t size() const
    {
        return columns.size();
    }

    bool empty() const
    {
        return columns.empty();
    }

    bool isBitEngineKeyStringColumn(const String & column_name) const;

    String toDebugString() const;

    /// Keep the sequence of columns and allow to lookup by name.
    using ColumnsContainer = boost::multi_index_container<
        ColumnDescription,
        boost::multi_index::indexed_by<
            boost::multi_index::sequenced<>,
            boost::multi_index::ordered_unique<boost::multi_index::member<ColumnDescription, String, &ColumnDescription::name>>>>;

    using SubcolumnsContainter = boost::multi_index_container<
        NameAndTypePair,
        boost::multi_index::indexed_by<
            boost::multi_index::hashed_unique<boost::multi_index::member<NameAndTypePair, String, &NameAndTypePair::name>>,
            boost::multi_index::hashed_non_unique<boost::multi_index::const_mem_fun<NameAndTypePair, String, &NameAndTypePair::getNameInStorage>>>>;

private:
    ColumnsContainer columns;
    SubcolumnsContainter subcolumns;

    void modifyColumnOrder(const String & column_name, const String & after_column, bool first);
    void addSubcolumnsToList(NamesAndTypesList & source_list) const;

    void addSubcolumns(const String & name_in_storage, const DataTypePtr & type_in_storage);
    void removeSubcolumns(const String & name_in_storage);
};

/// Validate default expressions and corresponding types compatibility, i.e.
/// default expression result can be casted to column_type. Also checks, that we
/// don't have strange constructions in default expression like SELECT query or
/// arrayJoin function.
Block validateColumnsDefaultsAndGetSampleBlock(ASTPtr default_expr_list, const NamesAndTypesList & all_columns, ContextPtr context);
}
