-- load modules -------------------------------------------------------------------------------------
local cjson = require "cjson"
local cjson2 = cjson.new()
local cjson_safe = require "cjson.safe"

-- global variables ---------------------------------------------------------------------------------
local requests_json = "requests.json"
local addrs   = {}
local addrs_f = {}
local threads = {}	-- only for done() statistics
local counter = 0
local max_requests = 0	-- maximum request (per thread)

-- general functions --------------------------------------------------------------------------------
function to_integer(number)
  return math.floor(tonumber(number) or error("Could not cast '" .. tostring(number) .. "' to number.'"))
end

-- Load URL paths from a file
function load_request_objects_from_file(file)
  local data = {}
  local content

  -- Check if the file exists
  local f=io.open(file,"r")
  if f~=nil then
    content = f:read("*all")

    io.close(f)
  else
    local msg = "load_request_objects_from_file(): unable to open %s\n"
    io.write(msg:format(file))
    os.exit()
  end

  -- Translate Lua value to/from JSON
  data = cjson.decode(content)

  return data
end

-- wrk() functions ----------------------------------------------------------------------------------
function delay() -- [ms]
--  local msg = "delay(): delay.min=%s, delay.max=%s\n"
--  io.write(msg:format(delay_min, delay_max))

  return math.random(delay_min, delay_max)
end

function setup(thread)
  local addrs_append = function(host, port)
    for i, addr in ipairs(wrk.lookup(host, port)) do
      if wrk.connect(addr) then
        addrs[#addrs+1] = addr
      end
    end
  end

  if #addrs == 0 then
    requests_data = load_request_objects_from_file(requests_json)

    -- Check if at least one request was found in the requests_json file
    if #requests_data <= 0 then
      local msg = "setup(): no requests found in %s\n"
      io.write(msg:format(requests_json))
      os.exit()
    end

--    local msg = "setup(): host=%s; port=%d; req_data=%d\n"
    for i, req in ipairs(requests_data) do
--      io.write(msg:format(req.host, req.port, #requests_data))
       addrs_append(req.host, req.port)
       addrs_f[#addrs] = req.host_from
    end
  end

  local index = (counter % #addrs) + 1 	-- we can have more connections than threads
  counter = counter + 1
  req = requests_data[index]

  if req == nil then
    io.write("setup(): no live hosts to test against\n")
    os.exit()
  end

  thread:set("id", counter)
  thread:set("host", req.host)
  thread:set("port", req.port)
  thread:set("method", req.method)
  thread:set("path", req.path)
  thread:set("headers", req.headers)
  thread:set("body", req.body)
  thread:set("delay_min", req.delay.min)	-- minimum per thread delay between requests [ms]
  thread:set("delay_max", req.delay.max)	-- maximum per thread delay between requests [ms]

  thread.addr = addrs[index]
  thread.scheme = req.scheme
  thread.src_ip = addrs_f[index]

--  local msg = "setup(): wrk.scheme=%s, host=%s, port=%d, method=%s, index=%d, thread.addr=%s, thread.scheme=%s, thread.src_ip=%s, #addrs=%d\n"
--  io.write(msg:format(wrk.scheme, req.host, req.port, req.method, index, thread.addr, thread.scheme, thread.src_ip, #addrs))

  table.insert(threads, thread)
end

-- redefining wrk.init() --> for testing multiple hosts, faster than "Host" redefinitions in requests
function wrk.init(args)
  requests  = 0	-- how many requests was issued within a thread
  responses = 0	-- how many responses was received within a thread

  if #args >= 1 then
--    local msg = "args=%d; args[0]=%s, args[1]=%s, args[2]=%s\n"
--    io.write(msg:format(#args, args[0], args[1], args[2]))
    max_requests = to_integer(args[1])
  end

--  local msg = "wrk.init(): thread %d, wrk.scheme=%s, wrk.host=%s, wrk.port=%s\n"
--  io.write(msg:format(id, wrk.scheme, wrk.host, wrk.port))

  wrk.headers["Host"] = host

  if type(init) == "function" then
    init(args)
  end

  local req = wrk.format()
  wrk.request = function()
    return req
  end
end

-- Prints Latency based on Coordinated Omission (HdrHistogram)
-- ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
-- function done(summary, latency, requests)
--   for i, thread in ipairs(threads) do
--     local id        = thread:get("id")
--     local requests  = thread:get("requests")
--     local responses = thread:get("responses")
--     local msg = "# %d,%d,%d,%d,%d,%d\n"
--     io.write(msg:format(
--              id, requests, responses,
--              latency:percentile(90), latency:percentile(95), latency:percentile(99)))
--   end
-- end

function request()
  requests = requests + 1
  start_us = wrk.time_us()

  return wrk.format(method, path, headers, body)
end

function response(status, headers, body)
  responses = responses + 1

  local cont_len = headers["Content-Length"]
  if (cont_len == nil) then
    cont_len = 0
  end

  local msg = "%d,%d,%d,%d,%s %s://%s:%s%s,%d,%d\n"
  local time_us = wrk.time_us()
  local delay = time_us - start_us

  io.write(msg:format(start_us,delay,status,cont_len,method,wrk.thread.scheme,host,port,path,id,responses))
  io.flush()

  -- Stop after max_requests if max_requests is a positive number
  if (max_requests > 0) and (responses >= max_requests) then
    wrk.thread:stop()
  end
end
