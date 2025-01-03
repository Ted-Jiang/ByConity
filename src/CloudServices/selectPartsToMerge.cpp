/*
 * Copyright (2022) Bytedance Ltd. and/or its affiliates
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <CloudServices/selectPartsToMerge.h>

#include <Catalog/DataModelPartWrapper.h>
#include <CloudServices/CnchPartsHelper.h>
#include <MergeTreeCommon/MergeTreeMetaBase.h>
#include <Storages/MergeTree/DanceMergeSelector.h>
#include <Storages/MergeTree/SimpleMergeSelector.h>
#include <Storages/MergeTree/MergeSelectorAdaptiveController.h>
#include <Storages/StorageCnchMergeTree.h>

#include <boost/functional/hash.hpp>

namespace DB
{

static void groupPartsByColumnsMutationsCommitTime(const ServerDataPartsVector & parts, std::vector<ServerDataPartsVector> & part_ranges);

ServerSelectPartsDecision selectPartsToMerge(
    const MergeTreeMetaBase & data,
    std::vector<ServerDataPartsVector> & res,
    const ServerDataPartsVector & data_parts,
    const std::unordered_map<String, std::pair<UInt64, UInt64> > & unselectable_part_rows,
    ServerCanMergeCallback can_merge_callback,
    const SelectPartsToMergeSettings & settings,
    LoggerPtr log)
{
    const auto data_settings = data.getSettings();
    auto metadata_snapshot = data.getInMemoryMetadataPtr();

    if (data_parts.empty())
    {
        if (log)
            LOG_DEBUG(log, "There are no parts in the table");
        return ServerSelectPartsDecision::NOTHING_TO_MERGE;
    }

    size_t max_total_size_to_merge = settings.max_total_size_to_merge;
    size_t num_default_workers = settings.num_default_workers;
    bool aggressive = settings.aggressive;
    bool enable_batch_select = settings.enable_batch_select;
    bool final = settings.final;
    bool select_nonadjacent_parts_allowed = data_settings->cnch_merge_select_nonadjacent_parts.value;
    // bool merge_with_ttl_allowed = settings.merge_with_ttl_allowed

    time_t current_time = std::time(nullptr);

    IMergeSelector<ServerDataPart>::PartsRanges parts_ranges;

    /// StoragePolicyPtr storage_policy = data.getStoragePolicy(IStorage::StorageLocation::MAIN);
    /// Volumes with stopped merges are extremely rare situation.
    /// Check it once and don't check each part (this is bad for performance).
    /// bool has_volumes_with_disabled_merges = storage_policy->hasAnyVolumeWithDisabledMerges();

    size_t parts_selected_precondition = 0;

    const auto & config = data.getContext()->getConfigRef();
    size_t max_parts_to_break = config.getInt64("dance_merge_selector.max_parts_to_break", MERGE_MAX_PARTS_TO_BREAK);

    // split parts into buckets if current table is bucket table.
    std::unordered_map<Int64, ServerDataPartsVector> buckets;
    if (data.isBucketTable())
    {
        /// Do aggressive merge for bucket table. (try to merge all parts in the bucket to 1 part)
        aggressive = true;
        groupPartsByBucketNumber(data, buckets, data_parts);
    }
    else
        buckets.emplace(0, data_parts);

    for (auto & bucket: buckets)
    {
        std::vector<ServerDataPartsVector> part_ranges_before_split;
        if (select_nonadjacent_parts_allowed)
            groupPartsByColumnsMutationsCommitTime(bucket.second, part_ranges_before_split);
        else
            part_ranges_before_split.emplace_back(std::move(bucket.second));

        for (const auto & range_before_split: part_ranges_before_split)
        {
            const String * prev_partition_id = nullptr;
            /// Previous part only in boundaries of partition frame
            const ServerDataPartPtr * prev_part = nullptr;

            for (const auto & part : range_before_split)
            {
                const String & partition_id = part->info().partition_id;

                /// If select_nonadjacent_parts_allowed is true, DanceMergeSelector will reorder parts by rows
                bool need_split_by_max_parts_to_break = !select_nonadjacent_parts_allowed
                    && !parts_ranges.empty() && parts_ranges.back().size() >= max_parts_to_break;

                if (!prev_partition_id || partition_id != *prev_partition_id || need_split_by_max_parts_to_break)
                {
                    if (parts_ranges.empty() || !parts_ranges.back().empty())
                        parts_ranges.emplace_back();

                    /// New partition frame.
                    prev_partition_id = &partition_id;
                    prev_part = nullptr;
                }

                /// Check predicate only for the first part in each range.
                if (!prev_part)
                {
                    /* Parts can be merged with themselves for TTL needs for example.
                    * So we have to check if this part is currently being inserted with quorum and so on and so forth.
                    * Obviously we have to check it manually only for the first part
                    * of each partition because it will be automatically checked for a pair of parts. */
                    if (!can_merge_callback(nullptr, part))
                        continue;

                    /// This part can be merged only with next parts (no prev part exists), so start
                    /// new interval if previous was not empty.
                    if (!parts_ranges.back().empty())
                        parts_ranges.emplace_back();
                }
                else
                {
                    /// If we cannot merge with previous part we had to start new parts
                    /// interval (in the same partition)
                    if (!can_merge_callback(*prev_part, part))
                    {
                        /// Now we have no previous part
                        prev_part = nullptr;

                        /// Mustn't be empty
                        assert(!parts_ranges.back().empty());

                        /// Some parts cannot be merged with previous parts and also cannot be merged with themselves,
                        /// for example, merge is already assigned for such parts, or they participate in quorum inserts
                        /// and so on.
                        /// Also we don't start new interval here (maybe all next parts cannot be merged and we don't want to have empty interval)
                        if (!can_merge_callback(nullptr, part))
                            continue;

                        /// Starting new interval in the same partition
                        parts_ranges.emplace_back();
                    }
                }

                IMergeSelector<ServerDataPart>::Part part_info;
                part_info.size = part->part_model().size();
                time_t part_commit_time = TxnTimestamp(part->getCommitTime()).toSecond();
                auto p_part = part->tryGetPreviousPart();
                while (p_part)
                {
                    ++part_info.chain_depth;
                    part_info.size += p_part->part_model().size();
                    part_commit_time = TxnTimestamp(p_part->getCommitTime()).toSecond();
                    p_part = p_part->tryGetPreviousPart();
                }
                /// Consider the base part's age as the part chain's age,
                /// so that the merge selector will give it a better score.
                part_info.age = current_time > part_commit_time ? current_time - part_commit_time : 0;
                part_info.rows = part->rowsCount();
                part_info.level = part->info().level;
                part_info.data = part.get();
                /// TODO:
                /// part_info.ttl_infos = &part->ttl_infos;
                /// part_info.compression_codec_desc = part->default_codec->getFullCodecDesc();
                /// part_info.shall_participate_in_merges = has_volumes_with_disabled_merges ? part->shallParticipateInMerges(storage_policy) : true;
                part_info.shall_participate_in_merges = true;

                ++parts_selected_precondition;

                parts_ranges.back().emplace_back(part_info);

                prev_part = &part;
            }
        }
    }

    if (parts_selected_precondition == 0)
    {
        if (log)
            LOG_DEBUG(log, "No parts satisfy preconditions for merge");
        return ServerSelectPartsDecision::CANNOT_SELECT;
    }

    /*
    if (metadata_snapshot->hasAnyTTL() && merge_with_ttl_allowed && !ttl_merges_blocker.isCancelled())
    {
        IMergeSelector<ServerDataPart>::PartsRange parts_to_merge;

        /// TTL delete is preferred to recompression
        TTLDeleteMergeSelector delete_ttl_selector(
            next_delete_ttl_merge_times_by_partition,
            current_time,
            data_settings->merge_with_ttl_timeout,
            data_settings->ttl_only_drop_parts);

        parts_to_merge = delete_ttl_selector.select(parts_ranges, max_total_size_to_merge);

        future_parts.emplace_back();

        if (!parts_to_merge.empty())
        {
            future_parts.back().merge_type = MergeType::TTL_DELETE;
        }
        else if (metadata_snapshot->hasAnyRecompressionTTL())
        {
            TTLRecompressMergeSelector recompress_ttl_selector(
                next_recompress_ttl_merge_times_by_partition,
                current_time,
                data_settings->merge_with_recompression_ttl_timeout,
                metadata_snapshot->getRecompressionTTLs());

            parts_to_merge = recompress_ttl_selector.select(parts_ranges, max_total_size_to_merge);
            if (!parts_to_merge.empty())
                future_parts.back().merge_type = MergeType::TTL_RECOMPRESS;
        }

        auto parts = toDataPartsVector(parts_to_merge);
        if (log)
            LOG_DEBUG(log, "Selected {} parts from {} to {}", parts.size(), parts.front()->name(), parts.back()->name());
        future_parts.back().assign(std::move(parts));
        return ServerSelectPartsDecision::SELECTED;
    }
    */

    /// Always use dance merge selector for StorageCnchMergeTree.
    DanceMergeSelector::Settings merge_settings;
    merge_settings.loadFromConfig(config);
    /// Override value from table settings
    merge_settings.max_parts_to_merge_base = std::min(data_settings->cnch_merge_max_parts_to_merge, data_settings->max_parts_to_merge_at_once);
    merge_settings.max_total_rows_to_merge = data_settings->cnch_merge_max_total_rows_to_merge;
    /// make sure rowid could be represented in 4 bytes
    if (metadata_snapshot->hasUniqueKey())
    {
        auto & max_rows = merge_settings.max_total_rows_to_merge;
        if (!(0 < max_rows && max_rows <= std::numeric_limits<UInt32>::max()))
            max_rows = std::numeric_limits<UInt32>::max();
    }
    merge_settings.enable_batch_select = enable_batch_select;
    /// NOTE: Here final is different from aggressive.
    /// The selector may not allow to merge [p1, p2] even though there are only two parts and aggressive is set.
    /// When final is set, we will skip some check for range [0, max_end) so that it can be a candidate result.
    if (aggressive)
        merge_settings.min_parts_to_merge_base = 1;
    merge_settings.final = final;
    merge_settings.max_age_for_single_part_chain = data_settings->merge_with_ttl_timeout;
    merge_settings.select_nonadjacent_parts_allowed = select_nonadjacent_parts_allowed;
    auto merge_selector = std::make_unique<DanceMergeSelector>(merge_settings);

    /// Using adaptive controller
    if (auto expected_parts_number = data_settings->cnch_merge_expected_parts_number.value;
        expected_parts_number >= 0 && !aggressive && !final)
    {
        if (auto bg_task_stats = MergeTreeBgTaskStatisticsInitializer::instance().getOrCreateTableStats(data.getStorageID()))
        {
            if (expected_parts_number == 0)
                expected_parts_number = num_default_workers;

            if (expected_parts_number > 0)
            {
                UInt64 write_amplification_optimize_threshold = data_settings->cnch_merge_write_amplification_optimize_threshold.value;
                if (log)
                    LOG_TRACE(log, "Using adaptive controller, expected_parts_number is {}", expected_parts_number);
                auto adaptive_controller = std::make_shared<MergeSelectorAdaptiveController>(
                    data.isBucketTable(),
                    expected_parts_number,
                    write_amplification_optimize_threshold,
                    merge_settings.max_parts_to_merge_base.value);
                adaptive_controller->init(bg_task_stats, parts_ranges, unselectable_part_rows);
                merge_selector->setAdaptiveController(adaptive_controller);
            }
        }
    }

    auto ranges = merge_selector->selectMulti(parts_ranges, max_total_size_to_merge, nullptr);
    if (ranges.empty())
    {
        if (log)
            LOG_DEBUG(log, "Get empty result from merge selector.");
        return ServerSelectPartsDecision::CANNOT_SELECT;
    }

    for (auto & range : ranges)
    {
        /// Do not allow to "merge" part with itself for regular merges, unless it is a TTL-merge where it is ok to remove some values with expired ttl
        if (range.size() == 1)
        {
            /// double check.
            if (range.front().chain_depth == 0)
            {
                if (log)
                    LOG_ERROR(log, "merge selector returned only one part to merge {}, skip this range.",
                        static_cast<const ServerDataPart *>(range.front().data)->name());
                continue;
            }
            // throw Exception("Logical error: merge selector returned only one part to merge", ErrorCodes::LOGICAL_ERROR);
        }
        auto & emplaced_parts = res.emplace_back();
        emplaced_parts.reserve(range.size());
        for (auto & part : range)
            emplaced_parts.push_back(static_cast<const ServerDataPart *>(part.data)->shared_from_this());

        /// When enable selct nonadjacent parts, merge selector can sort parts by rows/size/age to get a
        /// better selection. After selection, we need to sort parts again to get right result part name.
        if (data_settings->cnch_merge_select_nonadjacent_parts.value)
            std::sort(emplaced_parts.begin(), emplaced_parts.end(), CnchPartsHelper::PartComparator<ServerDataPartPtr>{});
    }

    return ServerSelectPartsDecision::SELECTED;
}


void groupPartsByBucketNumber(const MergeTreeMetaBase & data, std::unordered_map<Int64, ServerDataPartsVector> & grouped_buckets, const ServerDataPartsVector & data_parts)
{
    auto table_definition_hash = data.getTableHashForClusterBy();
    for (const auto & part : data_parts)
    {
        /// Can only merge those already been clustered parts.
        if (!table_definition_hash.match(part->part_model().table_definition_hash()))
            continue;
        if (auto it = grouped_buckets.find(part->part_model().bucket_number()); it != grouped_buckets.end())
            it->second.push_back(part);
        else
            grouped_buckets.emplace(part->part_model().bucket_number(), ServerDataPartsVector{part});
    }
}

static void groupPartsByColumnsMutationsCommitTime(const ServerDataPartsVector & parts, std::vector<ServerDataPartsVector> & part_ranges)
{
    using GroupKeyType = std::pair<UInt64, UInt64>;
    std::unordered_map<GroupKeyType, ServerDataPartsVector, boost::hash<GroupKeyType> > grouped_ranges;

    for (const auto & p: parts)
    {
        GroupKeyType key = std::make_pair(p->getColumnsCommitTime(), p->getMutationCommitTime());
        auto it = grouped_ranges.try_emplace(key).first;
        it->second.emplace_back(p);
    }

    for (auto & [_, range]: grouped_ranges)
    {
        part_ranges.emplace_back();
        std::swap(range, part_ranges.back());
    }
}

}
