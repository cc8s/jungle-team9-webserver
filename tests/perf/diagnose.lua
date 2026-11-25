-- Diagnostic run only: response callbacks affect achievable throughput.
local threads = {}
function setup(thread) table.insert(threads, thread) end
function init(args) status_counts = {} end
function response(status, headers, body)
  status_counts[status] = (status_counts[status] or 0) + 1
end
function done(summary, latency, requests)
  local counts = {}
  for _, thread in ipairs(threads) do
    for code, count in pairs(thread:get("status_counts") or {}) do
      counts[code] = (counts[code] or 0) + count
    end
  end
  local keys, fields = {}, {}
  for code, _ in pairs(counts) do table.insert(keys, code) end
  table.sort(keys)
  for _, code in ipairs(keys) do table.insert(fields, string.format('"%d":%d', code, counts[code])) end
  print(string.format('PERF_DIAGNOSTIC {"status":{%s},"requests":%d,"duration_us":%d,"connect_errors":%d,"read_errors":%d,"write_errors":%d,"timeout_errors":%d,"p50_us":%.3f,"p95_us":%.3f,"p99_us":%.3f}',
    table.concat(fields, ","), summary.requests, summary.duration,
    summary.errors.connect, summary.errors.read, summary.errors.write, summary.errors.timeout,
    latency:percentile(50), latency:percentile(95), latency:percentile(99)))
end
