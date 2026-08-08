#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
cat-watchdog 发送模块（独立Python进程）
通过 stdin 接收看门狗的 JSONL 消息，执行各类网络发送操作。
配置文件与看门狗共用 cw.json。
"""

import json
import sys
import os
import time
import threading
import requests
import websocket

# ========== 加载配置 ==========
CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cw.json")

def load_config():
    with open(CONFIG_PATH, "r", encoding="utf-8") as f:
        return json.load(f)

config = load_config()

notifier_cfg = config.get("notifier", {})
ws_url = notifier_cfg.get("ws_url", "")
ws_key = notifier_cfg.get("ws_key", "")
http_url = notifier_cfg.get("http_url", "")
http_key = notifier_cfg.get("http_key", "")

onebot_cfg = config.get("onebot", {})
onebot_enabled = onebot_cfg.get("enabled", False)
onebot_ws_url = onebot_cfg.get("ws_url", "ws://127.0.0.1:6700")
group_ids = onebot_cfg.get("group_ids", [])
user_ids = onebot_cfg.get("user_ids", [])
if isinstance(group_ids, int):
    group_ids = [group_ids]
if isinstance(user_ids, int):
    user_ids = [user_ids]

# ========== 连接状态 ==========
notifier_ws = None
notifier_ws_lock = threading.Lock()
onebot_ws = None
onebot_ws_lock = threading.Lock()
self_id = ""
running = True

# ========== 日志 ==========
def log(msg):
    ts = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
    print(f"[{ts}] [sender] {msg}", flush=True)

# ========== Notifier WS ==========
def notifier_on_message(ws, message):
    try:
        data = json.loads(message)
        log(f"Notifier收到消息: {json.dumps(data, ensure_ascii=False)}")
    except:
        pass

def notifier_on_error(ws, error):
    log(f"Notifier WS错误: {error}")

def notifier_on_close(ws, close_status_code, close_msg):
    log(f"Notifier WS关闭: {close_status_code} {close_msg}")

def notifier_on_open(ws):
    log(f"Notifier WS连接成功: {ws_url}")
    auth = json.dumps({"type": "auth", "key": ws_key})
    ws.send(auth)

def connect_notifier_ws():
    global notifier_ws
    try:
        ws = websocket.WebSocketApp(
            ws_url,
            on_message=notifier_on_message,
            on_error=notifier_on_error,
            on_close=notifier_on_close,
        )
        # 注入 on_open
        old_run = ws.run_forever
        def patched_run(*a, **kw):
            ws.on_open = notifier_on_open
            return old_run(*a, **kw)
        ws.run_forever = patched_run
        
        t = threading.Thread(target=ws.run_forever, daemon=True)
        t.start()
        notifier_ws = ws
        time.sleep(1)  # 等待握手
        return True
    except Exception as e:
        log(f"Notifier WS连接失败: {e}")
        return False

def send_notifier(message):
    """发送消息到 notifier（WS优先，HTTP备用）"""
    if notifier_ws:
        try:
            with notifier_ws_lock:
                notifier_ws.send(json.dumps(message))
            return True
        except Exception as e:
            log(f"Notifier WS发送失败，回退HTTP: {e}")
    
    # HTTP 备用
    if http_url:
        try:
            headers = {"Content-Type": "application/json", "X-Auth-Key": http_key}
            resp = requests.post(http_url, json=message, headers=headers, timeout=10)
            return resp.status_code in (200, 201, 204)
        except Exception as e:
            log(f"Notifier HTTP发送失败: {e}")
    return False

# ========== OneBot WS ==========
def onebot_on_message(ws, message):
    global self_id
    try:
        data = json.loads(message)
        # 获取自身ID
        if data.get("echo") == "get_self_id" and "data" in data:
            self_id = data["data"].get("user_id", "")
            log(f"OneBot自身ID: {self_id}")
        log(f"OneBot收到: {json.dumps(data, ensure_ascii=False)}")
    except:
        pass

def onebot_on_error(ws, error):
    log(f"OneBot WS错误: {error}")

def onebot_on_close(ws, close_status_code, close_msg):
    log(f"OneBot WS关闭: {close_status_code} {close_msg}")

def onebot_on_open(ws):
    global self_id
    log(f"OneBot WS连接成功: {onebot_ws_url}")
    # 获取自身ID
    payload = json.dumps({
        "action": "get_login_info",
        "params": {},
        "echo": "get_self_id"
    })
    ws.send(payload)

def connect_onebot_ws():
    global onebot_ws
    try:
        ws = websocket.WebSocketApp(
            onebot_ws_url,
            on_message=onebot_on_message,
            on_error=onebot_on_error,
            on_close=onebot_on_close,
        )
        old_run = ws.run_forever
        def patched_run(*a, **kw):
            ws.on_open = onebot_on_open
            return old_run(*a, **kw)
        ws.run_forever = patched_run
        
        t = threading.Thread(target=ws.run_forever, daemon=True)
        t.start()
        onebot_ws = ws
        time.sleep(1)
        return True
    except Exception as e:
        log(f"OneBot WS连接失败: {e}")
        return False

def onebot_send_group(group_id, message):
    """发送群消息"""
    if not onebot_ws:
        return False
    try:
        payload = json.dumps({
            "action": "send_group_msg",
            "params": {"group_id": group_id, "message": str(message)},
            "echo": f"send_group_{group_id}_{int(time.time())}"
        })
        with onebot_ws_lock:
            onebot_ws.send(payload)
        return True
    except Exception as e:
        log(f"发送群消息失败 {group_id}: {e}")
        return False

def onebot_send_private(user_id, message):
    """发送私聊消息"""
    if not onebot_ws:
        return False
    try:
        payload = json.dumps({
            "action": "send_private_msg",
            "params": {"user_id": user_id, "message": str(message)},
            "echo": f"send_private_{user_id}_{int(time.time())}"
        })
        with onebot_ws_lock:
            onebot_ws.send(payload)
        return True
    except Exception as e:
        log(f"发送私聊消息失败 {user_id}: {e}")
        return False

def onebot_send_all(message):
    """发送到所有配置的群和私聊"""
    success = True
    for gid in group_ids:
        if not onebot_send_group(gid, message):
            success = False
    for uid in user_ids:
        if not onebot_send_private(uid, message):
            success = False
    return success

def format_process_status(data):
    """格式化进程状态文本"""
    name = data.get("name", "unknown")
    running = data.get("running", False)
    pid = data.get("pid", 0)
    icon = "✅" if running else "❌"
    status = "运行中" if running else "已停止"
    pid_str = str(pid) if pid else "未知"
    ts = data.get("time", time.strftime("%Y-%m-%d %H:%M:%S", time.localtime()))
    return f"{icon} 进程状态变更: {name}\n状态: {status}\nPID: {pid_str}\n时间: {ts}"

def format_crash(data):
    """格式化崩溃通知"""
    name = data.get("name", "unknown")
    pid = data.get("pid", 0)
    restart_count = data.get("restart_count", 0)
    crash_reason = data.get("crash_reason", "未知")
    pid_str = str(pid) if pid else "未知"
    ts = data.get("time", time.strftime("%Y-%m-%d %H:%M:%S", time.localtime()))
    return f"💀 进程崩溃: {name}\nPID: {pid_str}\n原因: {crash_reason}\n已重启: {restart_count}次\n时间: {ts}"

def format_half_start(data):
    """格式化半启动通知"""
    name = data.get("name", "unknown")
    ports = data.get("ports", [])
    port_status = data.get("port_status", [])
    ts = data.get("time", time.strftime("%Y-%m-%d %H:%M:%S", time.localtime()))
    port_info = ""
    for p, s in zip(ports, port_status):
        icon = "🟢" if s else "🔴"
        port_info += f"  端口 {p}: {icon}\n"
    return f"⚠️ 半启动状态: {name}\n{port_info}时间: {ts}"

def format_watchdog_start(data):
    """格式化看门狗启动通知"""
    ts = data.get("time", time.strftime("%Y-%m-%d %H:%M:%S", time.localtime()))
    msg = data.get("message", "看门狗已启动")
    return f"🐱 {msg}\n时间: {ts}"

# ========== 消息分发 ==========
def handle_message(msg):
    """处理来自看门狗的消息"""
    msg_type = msg.get("type", "")
    data = msg.get("data", {})
    ts = msg.get("timestamp", data.get("time", time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())))
    
    if msg_type == "watchdog_start":
        text = format_watchdog_start(data)
        send_notifier(msg)
        if onebot_enabled:
            onebot_send_all(text)
    
    elif msg_type == "crash":
        data["time"] = ts
        text = format_crash(data)
        send_notifier(msg)
        if onebot_enabled:
            onebot_send_all(text)
    
    elif msg_type == "half_start":
        data["time"] = ts
        text = format_half_start(data)
        send_notifier(msg)
        if onebot_enabled:
            onebot_send_all(text)
    
    elif msg_type == "manual_restart":
        name = data.get("name", "unknown")
        reason = data.get("reason", "未知")
        ts_local = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
        text = f"🔄 手动重启: {name}\n原因: {reason}\n时间: {ts_local}"
        send_notifier(msg)
        if onebot_enabled:
            onebot_send_all(text)
    
    elif msg_type == "process_change":
        data["time"] = ts
        text = format_process_status(data)
        send_notifier(msg)
        if onebot_enabled:
            onebot_send_all(text)
    
    elif msg_type == "status_report":
        """完整状态报告"""
        running_count = data.get("running_count", 0)
        total_count = data.get("total_count", 0)
        process_list = data.get("process_list", "")
        hostname = data.get("hostname", "unknown")
        text = f"📊 看门狗状态报告\n主机: {hostname}\n运行: {running_count}/{total_count}\n{process_list}\n时间: {ts}"
        send_notifier(msg)
        if onebot_enabled:
            onebot_send_all(text)
    
    elif msg_type == "custom":
        """自定义消息"""
        text = data.get("text", "")
        targets = data.get("targets", ["notifier", "onebot"])
        if "notifier" in targets:
            send_notifier(msg)
        if "onebot" in targets and onebot_enabled:
            onebot_send_all(text)
    
    elif msg_type == "restart_request":
        """重启请求（通知看门狗执行）"""
        send_notifier(msg)
        log(f"收到重启请求: {data.get('name', '')}")
    
    elif msg_type == "ping":
        """心跳ping，回复pong"""
        response = {"type": "pong", "timestamp": ts}
        print(json.dumps(response), flush=True)
    
    else:
        log(f"未知消息类型: {msg_type}")
        send_notifier(msg)

# ========== 主循环 ==========
def main():
    global running
    
    log("发送器启动")
    log(f"配置加载: Notifier={bool(ws_url or http_url)}, OneBot={onebot_enabled}")
    
    # 连接Notifier WS
    if ws_url:
        connect_notifier_ws()
    
    # 连接OneBot WS
    if onebot_enabled:
        connect_onebot_ws()
    
    log("等待看门狗消息...")
    
    # 从 stdin 读取 JSONL
    for line in sys.stdin:
        if not running:
            break
        line = line.strip()
        if not line:
            continue
        
        try:
            msg = json.loads(line)
            handle_message(msg)
        except json.JSONDecodeError as e:
            log(f"JSON解析失败: {e}, 原始: {line[:100]}")
        except Exception as e:
            log(f"处理消息异常: {e}")

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        log("发送器收到中断，退出")
        running = False
    except Exception as e:
        log(f"发送器异常退出: {e}")
        running = False
