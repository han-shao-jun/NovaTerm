#pragma once
// Internal statements shared with the query-plan guard, not an application API.
namespace NovaTerm::Recording::IndexSql {
inline constexpr char RangeQuery[] =
    "SELECT chunk, offset, id, last_id FROM records WHERE id>=? AND id<=? ORDER BY id LIMIT ?";
inline constexpr char PrecedingQuery[] =
    "SELECT chunk, offset, id, last_id FROM records WHERE id<? ORDER BY id DESC LIMIT 1";
}
