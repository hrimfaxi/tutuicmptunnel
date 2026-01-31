#!/usr/bin/lua
--[[
  TutuICMPTunnel IP-sync script for OpenWrt (Lua 5.1)
  - No external library dependencies needed (like JSON libs)
  - Uses a simple text file for state persistence
  - Requires: lua luci-lib-nixio
--]]

local nixio_ok, nixio = pcall(require, "nixio")
if not nixio_ok or not nixio then
  error("require 'nixio' failed")
end

local function xclose(fd)
  if fd and fd:fileno() > 2 then
    fd:close()
  end
end

local function run_stdin_capture_stdout_stderr(cmd_table, stdin_data)
  assert(type(cmd_table) == "table" and #cmd_table >= 1, "cmd_table must be a table with at least one element")
  stdin_data = stdin_data or ""

  -- 创建 pipe 用于写入子进程 stdin
  local in_r, in_w = nixio.pipe()
  if not in_r then
    return "", false
  end

  -- 创建 pipe 用于从子进程 stdout读取
  local out_r, out_w = nixio.pipe()
  if not out_r then
    return "", false
  end

  local pid = nixio.fork()
  if pid == 0 then
    -- child: stdin -> in_r，stderr/stdout -> out_w
    if in_r then
      nixio.dup(in_r, nixio.stdin)
    else
      local nullr = nixio.open("/dev/null", "r")
      if nullr then
        nixio.dup(nullr, nixio.stdin)
        xclose(nullr)
      end
    end

    xclose(in_w)
    xclose(in_r)

    nixio.dup(out_w, nixio.stdout)
    nixio.dup(out_w, nixio.stderr)
    xclose(out_w)
    xclose(out_r)

    nixio.exec(unpack(cmd_table))

    -- exec 失败
    os.exit(127)
  end

  -- parent: 关闭 child 用的读端（父进程不需要读）
  xclose(in_r)

  -- parent: 关闭写端 (父进程不需要写)
  xclose(out_w)

  -- 向子进程写入 stdin_data（如果有写端）
  if in_w then
    local pos, len = 1, #stdin_data
    while pos <= len do
      local chunk = stdin_data:sub(pos, pos + 4095)
      local wrote, err = in_w:write(chunk)
      if not wrote then
        -- 写失败：关闭写端并等待子进程，返回 false
        xclose(in_w)
        nixio.waitpid(pid)
        return "", false
      end
      pos = pos + wrote
    end
    -- 写完后关闭写端以发送 EOF
    xclose(in_w)
  end

  -- 读取 stdout
  local chunks = {}
  while true do
    local data, err = out_r:read(4096)
    if not data or #data == 0 or err ~= nil then
      break
    else
      chunks[#chunks + 1] = data
    end
  end
  xclose(out_r)

  -- 等待子进程结束并获取退出码
  local _, raw_status, exit_code = nixio.waitpid(pid)
  local ok = (raw_status == "exited" and exit_code == 0)

  return table.concat(chunks), ok
end

local function run_capture_stdout(argv)
  assert(type(argv) == "table" and #argv >= 1, "argv must be a non-empty table")

  -- 创建 stdout 管道：子进程写 -> 父进程读
  local r, w = nixio.pipe()
  if not r then
    return "", false
  end

  local pid = nixio.fork()
  if pid == 0 then
    -- 子进程
    -- 将 stdout 重定向到管道写端
    nixio.dup(w, nixio.stdout)  -- fd 1
    -- 关闭子进程不需要的端
    if r then r:close() end
    if w then w:close() end

    -- 2>/dev/null
    local nullw = nixio.open("/dev/null", "w+")
    if nullw then
      nixio.dup(nullw, nixio.stderr)
      xclose(nullw)
    end

    -- 执行命令（按 PATH 搜索）；失败则以 127 退出
    nixio.execp(unpack(argv))
    os.exit(127)
  elseif pid < 0 then
    -- fork 失败
    if r then r:close() end
    if w then w:close() end
    return "", false
  end

  -- 父进程：关闭写端，只保留读端
  if w then w:close() end

  -- 读取 stdout
  local chunks = {}
  while true do
    local data, err = r:read(4096)
    if not data or #data == 0 or err ~= nil then
      break
    else
      chunks[#chunks + 1] = data
    end
  end
  r:close()

  -- 等待子进程结束并获取退出码
  local _, raw_status, exit_code = nixio.waitpid(pid)
  local ok = (raw_status == "exited" and exit_code == 0)

  return table.concat(chunks), ok
end

-- 配置部分 ---
-- 优先使用环境变量，否则使用下面的默认值
local config = {
  TUTU_UID      = os.getenv("TUTU_UID") or "tutu_client_name",
  HOST          = os.getenv("HOST") or "example.com",
  COMMENT       = os.getenv("COMMENT") or "tutu_client_name",
  PORT          = os.getenv("PORT") or "3322",
  SERVER_PORT   = os.getenv("SERVER_PORT") or "14801",
  PSK           = os.getenv("PSK") or "testpassword",
  -- 状态文件路径，推荐放在 /var/tmp 或 /tmp，因为它们在内存中，不会磨损闪存
  STATE_FILE    = os.getenv("STATE_FILE") or "/var/tmp/tutu_ip_state.txt",
  -- 强制更新时间（秒）
  FORCE_UPDATE_INTERVAL = tonumber(os.getenv("FORCE_UPDATE_INTERVAL") or "3600"),
  -- 获取公网IP的命令。
  IP_FETCH_CMD  = {
    "wget", "http://ip.3322.net", "-O", "-",
  }
}

-- 简单的日志函数，带时间戳
local function log(message)
  print(os.date("[%Y-%m-%d %H:%M:%S] ") .. message)
end

-- 去除字符串两端的空白字符
local function trim(s)
  return s:match("^%s*(.-)%s*$")
end

-- 解析命令行参数：-f/--force 强制同步（忽略state与时间间隔）
local function parse_args(argv)
  local opts = { force = false }
  argv = argv or {}
  for _, a in ipairs(argv) do
    if a == "-f" or a == "--force" then
      opts.force = true
    end
  end
  return opts
end

-- 获取当前公网IP
local function get_current_public_ip()
  log("正在获取公网IP...")
  local ip, ok = run_capture_stdout(config.IP_FETCH_CMD)
  if not ok then
    log("错误: 执行IP获取命令失败。")
    return nil
  end

  ip = trim(ip)
  -- 简单的IP格式校验
  if ip and ip:match("^[0-9]+%.[0-9]+%.[0-9]+%.[0-9]+$") then
    return ip
  else
    log("错误: 未能获取到有效的公网IP。收到: '" .. (ip or "nil") .. "'")
    return nil
  end
end

-- 读取上一次的状态
local function read_last_state()
  local state_file = assert(io.open(config.STATE_FILE, "r"), "无法打开状态文件进行读取，但这通常是正常的，表示首次运行。")
  if not state_file then
    -- 文件不存在，这是第一次运行
    return { last_ip = nil, last_update_timestamp = 0 }
  end
  local ip = state_file:read("*l") or "" -- 读取第一行 (IP)
  local timestamp = tonumber(state_file:read("*l") or "0") -- 读取第二行 (时间戳)
  state_file:close()
  return { last_ip = trim(ip), last_update_timestamp = timestamp }
end

-- 写入新的状态
local function write_new_state(ip_address, timestamp)
  log("正在更新状态文件: " .. config.STATE_FILE)
  local state_file = io.open(config.STATE_FILE, "w")
  if not state_file then
    log("严重错误: 无法写入状态文件 " .. config.STATE_FILE)
    return
  end
  state_file:write(ip_address .. "\n")
  state_file:write(timestamp .. "\n")
  state_file:close()
  log("状态文件更新成功。")
end

local function contains_update(msg, search)
  return string.find(msg, search, 1, true) ~= nil
end

-- 执行tutu客户端更新命令
local function run_update_command(current_ip)
  log("需要更新，正在准备执行命令...")

  local cmd = {
    "/usr/bin/tuctl_client",
    "-4", -- 默认使用ipv4 UDP
    "max-retries", "1",
    "psk", config.PSK,
    "server", config.HOST,
    "server-port", config.SERVER_PORT,
  }
  local input_str = string.format(
    'server-add uid %s address %s port %s comment "%s"\n',
    config.TUTU_UID, current_ip, config.PORT, config.COMMENT
  )

  local msg, ok = run_stdin_capture_stdout_stderr(cmd, input_str)
  log("服务器返回: " .. trim(msg))
  if ok and msg and contains_update(msg, "server updated") then
    return true
  end
  return false
end

-- 主逻辑 ---
local function main()
  local opts = parse_args(arg)

  log("--- 开始智能同步Tutu IP (Lua) ---")

  local current_ip = get_current_public_ip()
  if not current_ip then
    log("无法获取当前公网IP，退出。")
    return
  end

  local last_state
  -- 使用pcall来安全地读取文件，避免因文件不存在而报错退出
  local ok, state = pcall(read_last_state)
  if ok then
    last_state = state
  else
    log("警告: 读取状态文件失败 (可能是首次运行), 将强制更新。")
    last_state = { last_ip = nil, last_update_timestamp = 0 }
  end

  log("当前公网IP: " .. current_ip)
  log("上次记录IP: " .. (last_state.last_ip or "N/A"))

  local now = os.time()
  local should_update = false
  local reason = ""

  if opts.force then
    should_update = true
    reason = "命令行指定强制同步(-f/--force)"
  elseif current_ip ~= last_state.last_ip then
    should_update = true
    reason = "IP地址已变更"
  elseif (now - last_state.last_update_timestamp) > config.FORCE_UPDATE_INTERVAL then
    should_update = true
    reason = "超过阀值未更新，执行强制同步"
  end

  if should_update then
    log("需要更新，原因: " .. reason)
    local success = run_update_command(current_ip)
    if success then
      -- 只有命令成功才更新状态文件
      write_new_state(current_ip, now)
    else
      log("由于命令执行失败，未更新本地状态文件。")
    end
  else
    log("IP未变化且未到强制更新时间，无需操作。")
  end

  log("--- 同步任务完成 ---\n")
end

main()

-- vim: set tabstop=2 sw=2 expandtab:
