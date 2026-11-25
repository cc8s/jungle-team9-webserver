-- wrk -s validate.lua URL -- EXPECTED_FILE [identity]
-- identity: one distinct fixture per wrk thread, not per physical connection.
local threads = {}
local next_id = 0
function setup(thread)
  next_id = next_id + 1
  thread:set("fixture_id", next_id)
  table.insert(threads, thread)
end
function init(args)
  checked, mismatches = 0, 0
  local path = assert(args[1], "expected fixture file is required")
  if args[2] == "identity" then
    path = path:gsub("small.bin$", "client-" .. fixture_id .. ".bin")
    wrk.path = "/perf/client-" .. fixture_id .. ".bin"
    local request = wrk.format()
    wrk.request = function() return request end
  end
  local file = assert(io.open(path, "rb"))
  expected = file:read("*all")
  file:close()
end
local function header(headers, name)
  for key, value in pairs(headers) do
    if key:lower() == name then return value end
  end
end
function response(status, headers, body)
  checked = checked + 1
  if status ~= 200 or body ~= expected or tonumber(header(headers, "content-length")) ~= #expected then
    mismatches = mismatches + 1
  end
end
function done(summary, latency, requests)
  local total, errors = 0, 0
  for _, thread in ipairs(threads) do
    total = total + (thread:get("checked") or 0)
    errors = errors + (thread:get("mismatches") or 0)
  end
  print(string.format('PERF_VALIDATE {"checked":%d,"mismatches":%d,"requests":%d,"duration_us":%d,"connect_errors":%d,"read_errors":%d,"write_errors":%d,"timeout_errors":%d}',
    total, errors, summary.requests, summary.duration,
    summary.errors.connect, summary.errors.read, summary.errors.write, summary.errors.timeout))
end
