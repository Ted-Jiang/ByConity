#pragma once
#include "Columns/IColumn.h"
#include <Common/Logger.h>
#include "Processors/Formats/IInputFormat.h"
#include "Storages/MergeTree/KeyCondition.h"
#include "config_formats.h"
#if USE_ORC
#    include <memory>
#    include <Core/Block.h>
#    include <IO/ReadBuffer.h>
#    include <Interpreters/Context.h>
#    include <Storages/SelectQueryInfo.h>
#    include <arrow/result.h>
#    include <arrow/status.h>
#    include <orc/Reader.hh>
#    include <orc/Vector.hh>
#    include "Formats/FormatSettings.h"
#    include "Interpreters/Context_fwd.h"
#    include "OrcCommon.h"
#    include "Processors/Chunk.h"
using Status = arrow::Status;
template <typename T>
using Result = arrow::Result<T>;

#    define CHUNKREADER
#    ifdef CHUNKREADER
namespace DB
{
struct ScanParams
{
    // CE releated.
    Block header;
    ReadBuffer * in = nullptr;
    SelectQueryInfo select_query_info;
    ContextPtr local_context = nullptr;
    FormatSettings format_settings;
    size_t range_start = 0;
    size_t range_length = 0;
    size_t chunk_size = 4096;

    ColumnMappingPtr column_mapping;
    // ORC releated.
    std::optional<std::string> orc_tail;
};


class OrcChunkReader;
struct FilterDescription;

class OrcScanner
{
public:
    explicit OrcScanner(ScanParams & scan_params);
    ~OrcScanner();
    Status init();
    Status initLazyColumn();

    Status readNext(Block & block);
    Status prepareFileReader();
    static void buildColumnNameToId(const Block & header, const orc::Type & root_type, std::map<std::string, int64_t> & column_name_to_id);
    ColumnPtr filterBlock(const Block & block, const SelectQueryInfo & query_info);
    static Block
    mergeBlock(Block & block, Chunk & left, Block & left_header, Chunk & right, Block & right_header); // merge left and right into block
    static Block mergeBlock(Block & block, Block & left_block, Chunk & right, Block & right_header); // merge left and right into block


private:
    Status initChunkReader();
    ScanParams scan_params;
    std::unique_ptr<orc::Reader> file_reader;
    std::unique_ptr<OrcChunkReader> chunk_reader;
    std::shared_ptr<KeyCondition> key_condition;

    // orc releated params.
    std::map<std::string, int64_t> column_name_to_id;
    std::map<int64_t, std::string> column_id_to_name;
    std::set<int64_t> active_indices;
    std::set<int64_t> lazy_indices;
    std::set<int64_t> lowcard_indices;
    std::set<int64_t> lowcardnull_indices;
    Block active_header;
    Block lazy_header;
    LoggerPtr logger = getLogger("OrcScanner");
};

struct ChunkReaderParams
{
    SelectQueryInfo * select_query_info;
    ContextPtr local_context = nullptr;
    std::set<int64_t> active_indices;
    std::set<int64_t> lazy_indices;
    std::set<int64_t> lowcard_indices;
    std::set<int64_t> lowcardnull_indices;
    Block active_header;
    Block lazy_header;
    Block header;
    size_t range_start = 0;
    size_t range_length = 0;
    std::shared_ptr<KeyCondition> key_condition = nullptr;
    orc::Reader * file_reader = nullptr;
    FormatSettings format_settings;
    std::map<int64_t, std::string> column_id_to_name;
    std::map<std::string, int64_t> column_name_to_id;
    size_t read_chunk_size = 8192 * 8;
};

class OrcChunkReader
{
public:
    explicit OrcChunkReader(ChunkReaderParams & chunk_reader_param);
    ~OrcChunkReader();

    Status prepareStripeReader();

    Status readNext(orc::RowReader::ReadPosition & read_position);
    Result<Chunk> getChunk(); // one stage
    Result<Chunk> getActiveChunk(); // two stages - first read
    Result<Chunk> getLazyChunk(); // two stages - seconds read
    bool useLazyLoad();
    Status lazySeekTo(size_t row_in_stripe);
    Status lazyReadNext(size_t num_values);

    Status lazyFilter(FilterDescription & filter, uint32_t true_size);

    Status init();

private:
    Status initBlock(); // init cnch block and coverter.
    Status initRowReader(); // init orc row reader and create batch

    // Status splitStringFilter(std::map<String, ASTPtr > &string_conds, ASTPtr & rest);
    // Status evalStringFilterOnDict(ASTPtr & string_cond, std::unordered_map<uint64_t, orc::StringDictionary *> & sdicts, ColumnPtr * dict_filter);

    ChunkReaderParams chunk_reader_params;
    Block active_block;
    Block lazy_block;
    std::unique_ptr<ORCColumnToCHColumn> active_orc_column_to_ch_column = nullptr;
    std::unique_ptr<ORCColumnToCHColumn> lazy_orc_column_to_ch_column = nullptr;
    std::unique_ptr<ORCColumnToCHColumn> orc_column_to_ch_column = nullptr;

    std::shared_ptr<KeyCondition> key_condition = nullptr;
    orc::RowReaderOptions row_reader_options;
    std::unique_ptr<orc::RowReader> row_reader = nullptr;
    std::unique_ptr<orc::ColumnVectorBatch> batch = nullptr;
    std::vector<int> active_fields;
    std::vector<int> lazy_fields;
    std::unique_ptr<orc::StripeInformation> stripe_info = nullptr;
    LoggerPtr logger = getLogger("OrcChunkReader");
};
}
#    endif
#endif
