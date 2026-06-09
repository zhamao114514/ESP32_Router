/* 
 * ESP32-S3 校园网热点共享路由器
 * 可露希尔特制版 v3.4.0（开源重构版）
 * ⚠️程序由ai编写，人工排版
 * ═══════════════════════════════════════════════════
 * 功能说明：
 *   ESP32-S3 同时连接校园网 WiFi 并开启一个热点，
 *   其他设备（手机/笔记本）连接此热点即可通过
 *   ESP32 共享上网。通过 MAC 克隆规避校园网
 *   多设备登录检测，自动完成 Portal 认证。
 * ═══════════════════════════════════════════════════
 * 
 * 使用前必做：
 *   1. 修改下方「🛠️ 用户配置区」的所有参数
 *   2. ⚠️ 不要在公开的 GitHub 仓库中包含你的账号密码！
 *      建议将本文件加入 .gitignore，或上传时删除密码字段
 *   3. 确保你的 ESP32 开发板支持 lwip_napt
 *      （在 Arduino IDE / PlatformIO 中启用 PSRAM 和 IPv6）
 * ═══════════════════════════════════════════════════
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_wifi.h>
#include <lwip/lwip_napt.h>
#include <lwip/tcpip.h>
#include <lwip/dns.h>
#include <lwip/ip_addr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ============================================================
// 🛠️ 用户配置区 —— 下载后请逐项修改以下参数
// ============================================================

const char* CAMPUS_SSID   = "XXXXXXXX";     // 校园网 WiFi 名称（SSID）
const char* CAMPUS_PASS   = "";                 // 校园网 WiFi 密码（一般为空，留空即可）
const char* AUTH_HOST     = "10.200.1.11";      // ⬅️ 改成你校园网的 Portal 服务器 IP
const int   AUTH_PORT     = 801;                // ⬅️ 改成对应的端口号
const char* USER_ACCOUNT  = "111111111";       // ⬅️ 改成你的校园网账号（学号）
const char* USER_PASSWORD = "222222222";     // ⬅️⚠️⚠️⚠️ 改成你的校园网密码
const char* AP_SSID       = "ESP32-IoT";        // 热点名称（可自定义）
const char* AP_PASS       = "11451411";         // ⚠️ 热点密码（至少8位，建议修改！默认11451411）

// ===== 手机MAC地址（克隆这个，让校园网以为是同一台设备）=====
const uint8_t CLONE_MAC[] = {0x11, 0x22, 0x33, 0xAA, 0xBB, 0xCC};    // ⬅️ 举例手机MAC地址为11:22:33:AA:BB:CC
// 将你已通过校园网认证的手机的 MAC 地址填写在此处
// ESP32 会伪装成这台设备，校园网就不会要求重新认证
// 如何获取手机 MAC 地址？
//   - 安卓：设置 → 关于手机 → 状态信息 → MAC 地址
//   - iPhone：设置 → 通用 → 关于本机 → Wi-Fi 地址
//   - 注意：请关闭"随机MAC地址(Wi-Fi隐私)"功能，使用真实MAC
//     或者直接填写你设备实际连接时使用的那个MAC

// ============================================================
// 🧠 以下为程序逻辑代码，⚠️第146行要改成校园网登录对应的地址⚠️
// ============================================================

bool portalAuthed = false;
unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_INTERVAL = 600000;

static SemaphoreHandle_t napt_sem = NULL;

static void do_napt(void *arg) {
  uint32_t ip = *(uint32_t*)arg;
  ip_napt_enable(ip, 1);
  if (napt_sem != NULL) xSemaphoreGive(napt_sem);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n===== ESP32-S3 校园网热点 v3.3.3 =====");

  // 先设置WiFi模式，再克隆MAC
  WiFi.mode(WIFI_AP_STA);
  
  // 克隆手机MAC地址（设置STA接口的MAC）
  esp_wifi_set_mac(WIFI_IF_STA, (uint8_t*)CLONE_MAC);
  Serial.print("📡 MAC已克隆: ");
  for (int i = 0; i < 6; i++) {
    Serial.printf("%02X", CLONE_MAC[i]);
    if (i < 5) Serial.print(":");
  }
  Serial.println();

  connectToCampus();
  portalAuthed = doPortalAuth();
  setupAP();
  setupNAT();

  Serial.println("\n✅ 启动完成！");
  Serial.printf("热点: %s | IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  Serial.printf("校园网IP: %s\n", WiFi.localIP().toString().c_str());
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[!] 断开，重连...");
    // 重连时再设置一次MAC
    esp_wifi_set_mac(WIFI_IF_STA, (uint8_t*)CLONE_MAC);
    connectToCampus();
    if (!portalAuthed) portalAuthed = doPortalAuth();
  }
  if (portalAuthed && (millis() - lastHeartbeat > HEARTBEAT_INTERVAL)) {
    lastHeartbeat = millis();
    if (!doHeartbeat()) {
      Serial.println("[!] 心跳失败，重新认证...");
      portalAuthed = doPortalAuth();
    }
  }
  delay(5000);
}

void connectToCampus() {
  Serial.printf("连接 %s ... ", CAMPUS_SSID);
  WiFi.begin(CAMPUS_SSID, CAMPUS_PASS);
  int t = 0;
  while (WiFi.status() != WL_CONNECTED && t < 30) {
    delay(1000); Serial.print("."); t++;
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("\n✅ 已连接，IP: %s\n", WiFi.localIP().toString().c_str());
  else
    Serial.println("\n❌ 连接失败！");
}

bool doPortalAuth() {
  if (WiFi.status() != WL_CONNECTED) return false;
  String ip = WiFi.localIP().toString();
  String url = "http://";
  url += AUTH_HOST; url += ":"; url += String(AUTH_PORT); url += "/eportal/";
  String body = "c=Portal&a=login&callback=dr1003&login_method=1";
  body += "&user_account=";   body += String(USER_ACCOUNT);
  body += "&user_password=";  body += String(USER_PASSWORD);
  body += "&wlan_user_ip=";   body += ip;
  body += "&wlan_user_ipv6=&wlan_user_mac=000000000000";
  body += "&wlan_ac_ip=&wlan_ac_name=";
  body += "&jsVersion=3.3.2&v=669";

  WiFiClient c; HTTPClient h;
  h.begin(c, url);
  h.addHeader("Content-Type", "application/x-www-form-urlencoded");
  h.addHeader("User-Agent", "Mozilla/5.0");
  h.addHeader("Referer", "http://10.200.1.11:801/eportal/");                  // ⬅️ 这一行也要改（屎山代码发力了😅）
  h.setTimeout(10000);
  int code = h.POST(body);
  String resp = h.getString();
  h.end();

  if (code == 302 || (code == 200 && resp.indexOf("success") >= 0)) {
    Serial.println("✅ 认证成功！"); lastHeartbeat = millis(); return true;
  } else if (code == 200 && resp.indexOf("已在线") >= 0) {
    Serial.println("✅ 已在线"); lastHeartbeat = millis(); return true;
  } else {
    Serial.printf("❌ 失败 (HTTP %d)\n", code); return false;
  }
}

bool doHeartbeat() {
  WiFiClient c; HTTPClient h;
  h.begin(c, "http://www.baidu.com"); h.setTimeout(5000);
  int code = h.GET(); h.end(); return code == 200;
}

void setupAP() {
  WiFi.softAPConfig(
    IPAddress(192, 168, 5, 1),
    IPAddress(192, 168, 5, 1),
    IPAddress(255, 255, 255, 0)
  );
  WiFi.softAP(AP_SSID, AP_PASS);
  
  // 公共DNS
  ip_addr_t dns_google = IPADDR4_INIT(0x08080808);
  ip_addr_t dns_114    = IPADDR4_INIT(0x72727272);
  dns_setserver(0, &dns_google);
  dns_setserver(1, &dns_114);
  
  Serial.printf("✅ 热点已开启: %s\n", AP_SSID);
}

void setupNAT() {
  IPAddress apIP = WiFi.softAPIP();
  static uint32_t napt_ip = (uint32_t)apIP;
  
  napt_sem = xSemaphoreCreateBinary();
  tcpip_callback(do_napt, &napt_ip);
  
  if (xSemaphoreTake(napt_sem, pdMS_TO_TICKS(5000)) == pdTRUE) {
    Serial.println("✅ NAT转发已启用");
  } else {
    Serial.println("⚠️ NAT启用超时");
  }
  
  vSemaphoreDelete(napt_sem);
  napt_sem = NULL;
}

// ============================================================
// 📄 文件说明
// ============================================================
// 
// 🔍 配置参数获取指南
// ═══════════════════════════════════════════════════════════
// 
// 本项目的所有配置集中在文件顶部「🛠️ 用户配置区」。
// 下面教你如何获取每一个参数：
// 
// ── AUTH_HOST_IP & AUTH_PORT（认证服务器地址和端口）──
//   🔹 方法1（最简单）：
//     1. 手机/电脑连接校园网 WiFi
//     2. 打开浏览器，随便访问一个网站（如 baidu.com）
//     3. 页面会自动跳转到校园网登录页面
//     4. 看地址栏！例如跳转到：http://10.200.1.11:801/eportal/
//        → IP = 10.200.1.11，端口 = 801
//   
//   🔹 方法2（Windows电脑）：
//     1. 连上校园网后，打开命令提示符（Win+R → cmd）
//     2. 输入 ipconfig，找到"默认网关"
//     3. 默认网关的 IP 通常就是 Portal 服务器地址
//     4. 端口一般是 801 或 8080，观察登录页 URL 确认
//   
//
// ── USER_ACCOUNT（校园网账号）──
//   就是你的学号/工号！学校网络中心开通的账号。
//
// ── USER_PASSWORD（校园网密码）──
//   对应的密码。⚠️ 上传 GitHub 前务必删除此行内容！
//
// ── CLONE_MAC_ADDR（手机 MAC 地址）──
//   🔹 安卓手机：
//     设置 → 关于手机 → 状态信息 → MAC 地址
//     （部分安卓11+默认使用随机MAC，建议关掉Wi-Fi隐私功能）
//   🔹 iPhone：
//     设置 → 通用 → 关于本机 → Wi-Fi 地址
//     （iOS 14+也有私有Wi-Fi地址，连接校园网时可关掉）
//   🔹 或者干脆：用连上校园网的那台手机
//     在登录页面查看"已登录设备"里显示的那个MAC
//
// ═══════════════════════════════════════════════════════════
//
// 硬件要求:
//   - 开发板: ESP32-S3 (推荐) 或 ESP32（没试过其他版本的开发板）
//   - 在 Arduino IDE 中选择: Tools → Board → ESP32S3 Dev Module
//   - 需在 tools → partition scheme 中选择 "Huge APP (3MB No OTA/1MB SPIFFS)"
//   - 建议启用 PSRAM (Tools → PSRAM → "Enabled")
//
// 所需库:
//   本代码使用 ESP32 Arduino Core 内置库，无需额外安装。
//   如果编译报 lwip_napt.h 找不到，请更新 ESP32 板支持包到 2.0.14+ 版本。
//
// 首次使用步骤:
//   1. 按照上方指南获取你校园网的各个参数
//   2. 修改「🛠️ 用户配置区」的所有配置项
//   3. 将 ESP32-S3 通过 USB 连接到电脑
//   4. 在 Arduino IDE 中选择正确的端口和板型
//   5. 编译并上传
//   6. 打开串口监视器（115200 baud）查看运行日志
//   7. 用手机连接热点 "ESP32-IoT"，密码 "12345678"
//   8. 如果一切正常，手机即可上网
//
// 常见问题排查:
//   Q: 串口输出乱码？
//   A: 检查串口监视器波特率是否为 115200
//   
//   Q: 连接校园网失败？
//   A: 检查 CAMPUS_SSID 和 CAMPUS_PASS 是否正确
//      
//   Q: 能连 WiFi 但认证失败？
//   A: 先确认 Portal 服务器地址/端口是否正确
//      方法：在电脑浏览器打开 http://你填的IP:你填的端口
//      如果能打开登录页面，说明地址对了
//      否则需要修改 AUTH_HOST_IP 和 AUTH_PORT_NUM
//   
//   Q: "账号或密码错误"？
//   A: 确认 USER_ACCOUNT 和 USER_PASSWORD 是否和
//      手动在浏览器登录时用的一致。先手动登录一次排除账号问题
//   
//   Q: 认证成功但热点不能上网？
//   A: 检查 NAT 日志（串口输出看有没有"✅ NAT转发已启用"）
//      如果显示超时，先忽略，等心跳检测看看能不能通
//      
//   Q: 连上热点但设备显示"无互联网连接或自动跳转到校园网登录界面"？
//   A: 可能是MAC克隆出现问题
//      拿被克隆MAC的手机连接热点正常登录一次就好（或许呢？反正我这样是可以的）
