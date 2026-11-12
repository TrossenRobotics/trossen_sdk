/**
 * @file lerobot_table.cpp
 * @brief Implementation of the shared data parquet helpers.
 */

#include "trossen_sdk/io/backends/lerobot_common/lerobot_table.hpp"

#include <algorithm>
#include <iostream>
#include <system_error>

#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>

namespace trossen::io::backends {

namespace fs = std::filesystem;

std::shared_ptr<arrow::Schema> make_data_schema(int action_dim, int obs_dim) {
  return arrow::schema({
    arrow::field("action", arrow::fixed_size_list(arrow::float32(), action_dim)),
    arrow::field("observation.state", arrow::fixed_size_list(arrow::float32(), obs_dim)),
    arrow::field("timestamp", arrow::float32()),
    arrow::field("frame_index", arrow::int64()),
    arrow::field("episode_index", arrow::int64()),
    arrow::field("index", arrow::int64()),
    arrow::field("task_index", arrow::int64()),
  });
}

std::shared_ptr<arrow::Table> build_episode_table(
  const AlignedEpisode& ep, int action_dim, int obs_dim, int episode_index, int task_index,
  int64_t global_from)
{
  arrow::FloatBuilder ts_b;
  auto obs_vb = std::make_shared<arrow::FloatBuilder>();
  arrow::FixedSizeListBuilder obs_b(arrow::default_memory_pool(), obs_vb, obs_dim);
  auto act_vb = std::make_shared<arrow::FloatBuilder>();
  arrow::FixedSizeListBuilder act_b(arrow::default_memory_pool(), act_vb, action_dim);
  arrow::Int64Builder frame_b, epi_b, idx_b, task_b;
  auto* obs_val = static_cast<arrow::FloatBuilder*>(obs_b.value_builder());
  auto* act_val = static_cast<arrow::FloatBuilder*>(act_b.value_builder());

  arrow::Status st;
  for (size_t i = 0; i < ep.frames.size(); ++i) {
    const auto& f = ep.frames[i];
    st &= ts_b.Append(f.timestamp_s);
    st &= obs_b.Append();
    for (double v : f.observation) st &= obs_val->Append(static_cast<float>(v));
    st &= act_b.Append();
    for (double v : f.action) st &= act_val->Append(static_cast<float>(v));
    st &= frame_b.Append(static_cast<int64_t>(i));
    st &= epi_b.Append(episode_index);
    st &= idx_b.Append(global_from + static_cast<int64_t>(i));
    st &= task_b.Append(task_index);
  }

  std::shared_ptr<arrow::Array> ts_a, obs_a, act_a, frame_a, epi_a, idx_a, task_a;
  st &= ts_b.Finish(&ts_a);
  st &= obs_b.Finish(&obs_a);
  st &= act_b.Finish(&act_a);
  st &= frame_b.Finish(&frame_a);
  st &= epi_b.Finish(&epi_a);
  st &= idx_b.Finish(&idx_a);
  st &= task_b.Finish(&task_a);
  if (!st.ok()) {
    std::cerr << "Error: Failed to build episode table: " << st.ToString() << "\n";
    return nullptr;
  }

  return arrow::Table::Make(make_data_schema(action_dim, obs_dim),
                            {act_a, obs_a, ts_a, frame_a, epi_a, idx_a, task_a});
}

bool write_parquet_file(const arrow::Table& table, const fs::path& path, bool store_schema) {
  auto report = [&path](const char* step, const arrow::Status& status) {
    std::cerr << "Error: Failed to " << step << " " << path << ": " << status.ToString() << "\n";
    return false;
  };
  auto fail = [&path, &report](const char* step, const arrow::Status& status) {
    std::error_code ec;
    fs::remove(path, ec);
    return report(step, status);
  };
  auto out_res = arrow::io::FileOutputStream::Open(path.string());
  if (!out_res.ok()) return report("open", out_res.status());
  auto props =
    parquet::WriterProperties::Builder().compression(parquet::Compression::SNAPPY)->build();
  parquet::ArrowWriterProperties::Builder arrow_builder;
  if (store_schema) arrow_builder.store_schema();
  const arrow::Status write_st =
    parquet::arrow::WriteTable(table, arrow::default_memory_pool(), *out_res,
                               std::max<int64_t>(1, table.num_rows()), props,
                               arrow_builder.build());
  const arrow::Status close_st = (*out_res)->Close();
  if (!write_st.ok()) return fail("write", write_st);
  if (!close_st.ok()) return fail("close", close_st);
  return true;
}

}  // namespace trossen::io::backends
