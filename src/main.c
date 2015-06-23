// BGAPI を用いて BLE デバイスの属性を検索するコード
// by bunji2

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <windows.h>

#include "cmd_def.h"
#include "uart.h"

//#define DEBUG

#define CLARG_PORT 1
#define CLARG_ACTION 2

#define UART_TIMEOUT 1000

#define FIRST_HANDLE 0x0001
#define LAST_HANDLE  0xffff

uint16 first_handle = FIRST_HANDLE;
uint16 last_handle = LAST_HANDLE;

#define MAX_DEVICES 64
int found_devices_count = 0;
bd_addr found_devices[MAX_DEVICES];

char str_buf[80] = {0x0};
enum actions {
  action_none,
  action_scan,
  action_connect,
  action_info,
};
enum actions action = action_none;

typedef enum {
  state_disconnected,
  state_connecting,
  state_connected,
  state_finish,
  state_last
} states;

states state = state_disconnected;

const char *state_names[state_last] = {
  "disconnected",
  "connecting",
  "connected",
  "finish"
};

uint16 type = 0x2800;

bd_addr connect_addr;

void usage(char *exe)
{
  printf("%s <list|COMx <info|scan|address 0xXXXX <0xXXXX 0xXXXX>>>\n", exe);
}

void change_state(states new_state)
{
#ifdef DEBUG
  printf("DEBUG: State changed: %s --> %s\n",
    state_names[state], state_names[new_state]);
#endif
  state = new_state;
}

/**
 * Compare Bluetooth addresses
 *
 * @param first First address
 * @param second Second address
 * @return Zero if addresses are equal
 */
int cmp_bdaddr(bd_addr first, bd_addr second)
{
  int i;
  for (i = 0; i < sizeof(bd_addr); i++) {
    if (first.addr[i] != second.addr[i]) return 1;
  }
  return 0;
}

void print_bdaddr(bd_addr bdaddr)
{
  printf("%02x:%02x:%02x:%02x:%02x:%02x",
      bdaddr.addr[5],
      bdaddr.addr[4],
      bdaddr.addr[3],
      bdaddr.addr[2],
      bdaddr.addr[1],
      bdaddr.addr[0]);
}

void print_raw_packet(struct ble_header *hdr, unsigned char *data)
{
  int i;
  printf("Incoming packet: ");
  for (i = 0; i < sizeof(*hdr); i++) {
    printf("%02x ", ((unsigned char *)hdr)[i]);
  }
  for (i = 0; i < hdr->lolen; i++) {
    printf("%02x ", data[i]);
  }
  printf("\n");
}

void output(uint8 len1, uint8* data1, uint16 len2, uint8* data2)
{
  if (uart_tx(len1, data1) || uart_tx(len2, data2)) {
    printf("ERROR: Writing to serial port failed\n");
    exit(1);
  }
}

int read_message(int timeout_ms)
{
  unsigned char data[256]; // enough for BLE
  struct ble_header hdr;
  int r;
  const struct ble_msg *msg;

  r = uart_rx(sizeof(hdr), (unsigned char *)&hdr, timeout_ms);
  if (!r) {
    return -1; // timeout
  }
  else if (r < 0) {
    printf("ERROR: Reading header failed. Error code:%d\n", r);
    return 1;
  }

  if (hdr.lolen) {
    r = uart_rx(hdr.lolen, data, timeout_ms);
    if (r <= 0) {
      printf("ERROR: Reading data failed. Error code:%d\n", r);
      return 1;
    }
  }

  msg = (const struct ble_msg *)ble_get_msg_hdr(hdr);

#ifdef DEBUG
  print_raw_packet(&hdr, data);
#endif

  if (!msg) {
    printf("ERROR: Unknown message received\n");
    exit(1);
  }

  msg->handler(data);

  return 0;
}

// [1] スキャンしたアドバタイズパケットの取得
void ble_evt_gap_scan_response(
  const struct ble_msg_gap_scan_response_evt_t *msg
){
  int i;
  char *name = NULL;
  uint8 type;

  if (found_devices_count >= MAX_DEVICES) change_state(state_finish);

  // Check if this device already found
  for (i = 0; i < found_devices_count; i++) {
    if (!cmp_bdaddr(msg->sender, found_devices[i])) return;
  }
  found_devices_count++;
  memcpy(found_devices[i].addr, msg->sender.addr, sizeof(bd_addr));

  // Parse data
  for (i = 0; i < msg->data.len; ) {
    int8 len = msg->data.data[i++];
    if (!len) continue;
    if (i + len > msg->data.len) break; // not enough data
    type = msg->data.data[i++];
    switch (type) {
    case 0x09: // AD Type == "Complete Local Name"
      name = malloc(len);
      memcpy(name, msg->data.data + i, len - 1);
      name[len - 1] = '\0';
    }

    i += len - 1;
  }

  // アドレス
  print_bdaddr(msg->sender);
  // RSSI
  printf(" RSSI:%d", msg->rssi);
  // Complete Local Name
  printf(" Name:%s", (name)?(name):"Unknown");
  printf("\n");

  free(name);
}

// [2] デバイスの情報表示の取得
void ble_rsp_system_get_info(
  const struct ble_msg_system_get_info_rsp_t *msg
){
  printf("#ble_rsp_system_get_info\n");
  printf("major=%u, minor=%u, ", msg->major, msg->minor);
  printf("patch=%u, ", msg->patch);
  printf("build=%u, ", msg->build);
  printf("ll_version=%u, ", msg->ll_version);
  printf("protocol_version=%u, ", msg->protocol_version);
  printf("hw=%u\n", msg->hw);
  if (action == action_info) change_state(state_finish);
}

// [3] BLE機器との接続
void ble_evt_connection_status(
  const struct ble_msg_connection_status_evt_t *msg
){

//  printf("#ble_evt_connection_status [%s]\n", state_names[state]);
  
  // 新規接続
  if (msg->flags & connection_connected) {
    uint8 uuid [2] = { 0x00, 0x00 };
    change_state(state_connected);
    printf("#Connected to ");
    print_bdaddr(connect_addr);
    printf("\n");
    
//    change_state(state_finish);
    
    uuid[1] = ((type & 0xFF00) >> 8);
    uuid[0] = (type & 0x00FF);

//printf("#type=%04hx\n", type);
//printf("#uuid[0]=%02hx\n", uuid[0]);
//printf("#uuid[1]=%02hx\n", uuid[1]);

    // 属性読み出し要求の送信
    ble_cmd_attclient_read_by_type(
      msg->connection, first_handle, last_handle, 2, uuid);
    printf("handle(dec),handle(hex),type,value\n");
  }
}

void print_str_uint8array(uint8array data) {
  int i;
  for (i=0; i<data.len; i++) {
    printf("%c", data.data[data.len-i-1]);
  }
}

void print_hex_uint8array(uint8 *data, uint8 len) {
  int i;
  for (i=0; i<len; i++) {
    printf("%02hx", data[i]);
  }
}

void print_hex_uint8array_rev(uint8 *data, uint8 len) {
  int i;
  for (i=0; i<len; i++) {
    printf("%02hx", data[len-i-1]);
  }
}

void print_att_properties(uint8 a) {
  int not_head = 0;
  printf("%02hx(", a);
  if (a & 0x01) {
    printf("Broadcast");
    not_head = 1;
  }
  if (a & 0x02) {
    if (not_head) { printf("|"); }
    printf("Read");
    not_head = 1;
  }
  if (a & 0x04) {
    if (not_head) { printf("|"); }
    printf("Write Without Response,");
    not_head = 1;
  }
  if (a & 0x08) {
    if (not_head) { printf("|"); }
    printf("Write");
    not_head = 1;
  }
  if (a & 0x10) {
    if (not_head) { printf("|"); }
    printf("Notify");
    not_head = 1;
  }
  if (a & 0x20) {
    if (not_head) { printf("|"); }
    printf("Indicate");
    not_head = 1;
  }
  if (a & 0x40) {
    if (not_head) { printf("|"); }
    printf("Signed Write Command");
    not_head = 1;
  }
  if (a & 0x80) {
    if (not_head) { printf("|"); }
    printf("Extended Property");
  }
  printf(")");
}
void ble_evt_attclient_attribute_value(
  const struct ble_msg_attclient_attribute_value_evt_t *msg
){
  int i;
  uint16 d;
  float x;
  if (msg->type != attclient_attribute_value_type_read_by_type) {
    return; // ignore except events of "read_by_type"
  }
//  printf("#ble_evt_attclient_attribute_value [%s]\n", state_names[state]);
//  printf("atthandle=0x%04hx, type=0x%02hx, ", msg->atthandle, msg->type);
  printf("%d, 0x%04hx, 0x%04hx, ", msg->atthandle, msg->atthandle, type);

  switch(type) {
  case 0x2803:
    printf("Properties=0x");
//    print_hex_uint8array(msg->value.data, 1);
    print_att_properties(msg->value.data[0]);
    printf(", Char_value_handle=0x");
    print_hex_uint8array_rev(msg->value.data+1, 2);
    printf(", Char_UUID=0x");
    print_hex_uint8array_rev(msg->value.data+3, msg->value.len-3);
    printf("\n");
    break;
  case 0x2800:
  case 0x2801:
    printf("Service_UUID=0x");
    print_hex_uint8array_rev(msg->value.data, msg->value.len);
    printf("\n");
    break;
  case 0x2901:
  case 0x2a00:
    printf("Value=");
    if (msg->value.len < 80) {
      memcpy(str_buf, msg->value.data, msg->value.len);
      printf("\"%s\"\n", str_buf);
    } else {
      printf("(too long! %d bytes)\n", msg->value.len);
    }
//    print_hex_uint8array(msg->value.data, msg->value.len);
//    print_str_uint8array(msg->value);
//    printf("\n");
    break;
  case 0x2902:
    d = (msg->value.data[1] << 8) + msg->value.data[0];
    printf("Value=");
    if (d == 0x0000) {
      printf("None");
    } else if (d == 0x0001) {
      printf("Notify");
    } else if (d == 0x0002) {
      printf("Indicate");
    } else {
      printf("Unknown");
    }
    printf("(0x%04hx)\n", d);
    break;
  case 0x2a04:
    d = (msg->value.data[1]<<8)+msg->value.data[0];
//    printf("\nd=%d, d*1.25=%f\n", d, x);
    printf("connInterval=%d[ms]-", (int)((float)d * 1.25));
    d = (msg->value.data[3]<<8)+msg->value.data[2];
    printf("%d[ms]", (int)((float)d * 1.25));
    d = (msg->value.data[5]<<8)+msg->value.data[4];
//    printf("d=%u\n", d);
    printf(", Slave Latency=%d", d);
    d = (msg->value.data[7]<<8)+msg->value.data[6];
//    printf("d=%u\n", d);
    printf(", Supervision Timeout Multiplier=%d\n", d);
    break;
  default:
    printf("Value=0x");
    print_hex_uint8array(msg->value.data, msg->value.len);
    printf("\n");
  }
}

void ble_evt_attclient_procedure_completed(
  const struct ble_msg_attclient_procedure_completed_evt_t *msg
){
/*
  printf("#ble_evt_attclient_procedure_completed [%s]\n",
    state_names[state]);
  printf("#result=%d, chrhandle=0x%04hx\n", msg->result, msg->chrhandle);
*/

  if (msg->result != 0) {
    printf("[Error] result_code=%d, type=0x%04hx, first_handle=0x%04hx, last_handle=0x%04hx\n",
      msg->result, type, first_handle, last_handle);
  }
  change_state(state_finish);
}

// ここから、このコードでは使用していない関数
void ble_evt_attclient_group_found(
  const struct ble_msg_attclient_group_found_evt_t *msg
){
/*
  uint16 uuid;
  
  printf("#ble_evt_attclient_group_found [%s]\n", state_names[state]);

  if (msg->uuid.len == 0) return;
  uuid = (msg->uuid.data[1] << 8) | msg->uuid.data[0];

  printf("service=0x%04x, handles=%d-%d\n", uuid, msg->start, msg->end);
*/
}

void ble_evt_attclient_find_information_found(
  const struct ble_msg_attclient_find_information_found_evt_t *msg
){
/*
  printf("#ble_evt_attclient_find_information_found [%s]\n",
    state_names[state]);
*/
}


// ここまで、このコードでは使用していない関数

// 切断されたときのコールバック関数
void ble_evt_connection_disconnected(
  const struct ble_msg_connection_disconnected_evt_t *msg
) {
  change_state(state_disconnected);
  printf("Connection terminated, trying to reconnect\n");
  change_state(state_connecting);
  // [3] BLE機器に再接続しようとする
  ble_cmd_gap_connect_direct(
    &connect_addr, gap_address_type_public, 40, 60, 100,0);
}


// Ctrl-C 押下でスキャンを終了させる
static void handler(int sig)
{
  if (sig == SIGINT) {
    printf("Ctrl-C!\n");
    change_state(state_finish);
    if (action == action_scan) {
      ble_cmd_gap_end_procedure();
    }
  }
}

int main(int argc, char *argv[]) {
  char *uart_port = "";

  // シグナルハンドラの登録
  if (signal(SIGINT, handler) == SIG_ERR)
    return 1;
  
  // 引数が不足しているかのチェック
  if (argc <= CLARG_PORT) {
    usage(argv[0]);
    return 1;
  }

  // COM ポート引数のチェック
  if (argc > CLARG_PORT) {
    if (strcmp(argv[CLARG_PORT], "list") == 0) {
      uart_list_devices(); // デバイスのリスト表示
      return 1;
    }
    else {
      uart_port = argv[CLARG_PORT];
    }
  }

  // アクション引数のチェック
  if (argc > CLARG_ACTION) {
    int i;
    for (i = 0; i < strlen(argv[CLARG_ACTION]); i++) {
      // 小文字にしておく
      argv[CLARG_ACTION][i] = tolower(argv[CLARG_ACTION][i]);
    }

    if (strcmp(argv[CLARG_ACTION], "scan") == 0) {
      action = action_scan;    // スキャン
    } else if (strcmp(argv[CLARG_ACTION], "info") == 0) {
      action = action_info;    // デバイス情報の表示
    } else {
      int i;
      short unsigned int addr[6];
      if (sscanf(argv[CLARG_ACTION],
          "%02hx:%02hx:%02hx:%02hx:%02hx:%02hx",
          &addr[5],
          &addr[4],
          &addr[3],
          &addr[2],
          &addr[1],
          &addr[0]) == 6) { // アドレスのとき

        for (i = 0; i < 6; i++) {
          connect_addr.addr[i] = addr[i];
        }
        
        if (argc > CLARG_ACTION+1) { // 属性タイプ
          uint16 d=0;
          if (sscanf(argv[CLARG_ACTION+1], "0x%04hx", &d) == 1) {
            switch (d) {
            case 0x2800:
            case 0x2801:
            case 0x2803:
            case 0x2901:
            case 0x2902:
            case 0x2a00:
            case 0x2a01:
            case 0x2a02:
            case 0x2a03:
            case 0x2a04:
            case 0x2a05:
              type = d;
              break;
            default:
              type = 0x2800;
            }
          }
        }
        if (argc > CLARG_ACTION+2) { // 開始ハンドル
          uint16 d=0;
          if (sscanf(argv[CLARG_ACTION+2], "0x%04hx", &d) == 1) {
            first_handle = d;
          }
        }
        if (argc > CLARG_ACTION+3) { // 終了ハンドル
          uint16 d=0;
          if (sscanf(argv[CLARG_ACTION+3], "0x%04hx", &d) == 1) {
            last_handle = d;
          }
        }
        
        action = action_connect;
      }
    }
  }
  if (action == action_none) {
    usage(argv[0]);
    return 1;
  }

  bglib_output = output;

  // COMポートのオープン
  if (uart_open(uart_port)) {
    printf("ERROR: Unable to open serial port\n");
    return 1;
  }

  // BLED のリセット
  ble_cmd_system_reset(0);
  uart_close();
  do {
//    usleep(500000); // 0.5s
    Sleep(500); // 0.5s
  } while (uart_open(uart_port));

  // アクションの実行
  if (action == action_scan) {

    // [1] スキャンの開始
    ble_cmd_gap_discover(gap_discover_observation);

  } else if (action == action_info) {

    // [2] デバイスの情報表示の要求
    ble_cmd_system_get_info();

  } else if (action == action_connect) {

/*
    printf("Connecting to ");
    print_bdaddr(connect_addr);
    printf("\n");
*/
    change_state(state_connecting);
    // [3] BLE機器への接続
    ble_cmd_gap_connect_direct(
      &connect_addr, gap_address_type_public, 40, 60, 100,0);
  }

  // メッセージループ
  while (state != state_finish) {
    if (read_message(UART_TIMEOUT) > 0) break;
  }

  // COMポートのクローズ
  uart_close();

  return 0;
}
