/* ============================================
   FTP-like Server (Control:21 / Data:22)

   編譯：
   gcc server.c -o server

   執行（需要 root 因為使用 Port 21/22）：
   sudo ./server

   功能：
   1. 支援大檔案 (>4GB)
   2. 支援斷點續傳
   3. 支援多人連線 (fork)
   4. 限制同時連線人數

============================================ */

#define _GNU_SOURCE                    // 啟用 GNU 擴充功能
#define _LARGEFILE64_SOURCE           // 啟用 64-bit 檔案支援
#define _FILE_OFFSET_BITS 64          // 將 off_t 設為 64-bit

#include <stdio.h>                    // printf, perror
#include <stdlib.h>                   // exit
#include <string.h>                   // memset, strstr
#include <unistd.h>                   // close, fork
#include <arpa/inet.h>                // sockaddr_in, htons
#include <sys/socket.h>               // socket API
#include <sys/stat.h>                 // stat (檔案大小)
#include <sys/wait.h>                 // waitpid
#include <signal.h>                   // signal
#include <errno.h>                    // errno

#define CONTROL_PORT 21               // 控制通道 (FTP control)
#define DATA_PORT 22                  // 資料通道 (FTP data)
#define BUFFER_SIZE 8192              // buffer 大小
#define MAX_CLIENTS 3                 // 最大同時連線數

volatile sig_atomic_t active_clients = 0; // 當前連線數（可安全於 signal 使用）

/* ===== SIGCHLD handler：回收子進程 ===== */
void sigchld_handler(int s) {
    while (waitpid(-1, NULL, WNOHANG) > 0) { // 回收所有已結束子進程
        active_clients--;                    // 連線數 -1
    }
}

/* ===== Client 處理 ===== */
void handle_client(int control_sock, int data_sock) {

    char filename[256] = {0};               // 存檔名
    off_t offset = 0;                       // 續傳位置

    /* ---------- 控制通道 ---------- */

    if (recv(control_sock, filename, sizeof(filename), 0) <= 0) { // 接收檔名
        close(control_sock);              // 關閉 socket
        exit(0);                          // 結束子進程
    }

    if (strstr(filename, "..")) {         // 安全檢查（避免目錄穿越）
        printf("非法檔名\n");
        close(control_sock);
        exit(1);
    }

    struct stat st;                       // 檔案資訊結構
    if (stat(filename, &st) == 0) {       // 如果檔案存在
        offset = st.st_size;              // 取得目前檔案大小
    }

    if (send(control_sock, &offset, sizeof(offset), 0) != sizeof(offset)) { // 傳 offset
        perror("send offset failed");
        close(control_sock);
        exit(1);
    }

    /* ---------- 資料通道 ---------- */

    FILE *fp = fopen64(filename, "ab");   // 開檔（append + binary）
    if (!fp) {
        perror("file open failed");
        close(data_sock);
        exit(1);
    }

    char buffer[BUFFER_SIZE];             // 資料 buffer
    ssize_t n;                            // 接收大小

    while (1) {
        n = recv(data_sock, buffer, BUFFER_SIZE, 0); // 接收資料

        if (n > 0) {                      // 收到資料
            if (fwrite(buffer, 1, n, fp) != n) { // 寫入檔案
                perror("write error");
                break;
            }
        } else if (n == 0) {              // client 關閉
            break;
        } else {                          // 發生錯誤
            if (errno == EINTR) continue; // 被中斷就重試
            perror("recv error");
            break;
        }
    }

    printf("完成: %s\n", filename);       // 印出完成訊息

    fclose(fp);                           // 關檔
    close(control_sock);                  // 關控制通道
    close(data_sock);                     // 關資料通道

    exit(0);                              // 子進程結束
}

/* ===== 主程式 ===== */
int main() {

    int control_sock, data_sock;          // 控制與資料 socket
    struct sockaddr_in addr;              // 地址結構

    signal(SIGCHLD, sigchld_handler);     // 註冊 signal handler

    /* ===== 建立 Control Socket ===== */

    control_sock = socket(AF_INET, SOCK_STREAM, 0); // TCP socket

    int opt = 1;
    setsockopt(control_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); // 避免 port 卡住

    addr.sin_family = AF_INET;            // IPv4
    addr.sin_port = htons(CONTROL_PORT);  // 設定 port 21
    addr.sin_addr.s_addr = INADDR_ANY;    // 接受所有 IP

    bind(control_sock, (struct sockaddr*)&addr, sizeof(addr)); // 綁定
    listen(control_sock, 10);             // 開始監聽

    /* ===== 建立 Data Socket ===== */

    data_sock = socket(AF_INET, SOCK_STREAM, 0); // TCP socket

    setsockopt(data_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    addr.sin_port = htons(DATA_PORT);     // 設定 port 22

    bind(data_sock, (struct sockaddr*)&addr, sizeof(addr));
    listen(data_sock, 10);

    printf("Server 啟動 (21/22)\n");

    while (1) {

        if (active_clients >= MAX_CLIENTS) continue; // 超過人數限制

        int c_sock = accept(control_sock, NULL, NULL); // 接 control
        int d_sock = accept(data_sock, NULL, NULL);    // 接 data

        active_clients++;               // 人數 +1

        if (fork() == 0) {              // 建立子進程
            close(control_sock);        // 子進程不用監聽
            close(data_sock);
            handle_client(c_sock, d_sock); // 處理 client
        }

        close(c_sock);                  // 父進程關閉 client socket
        close(d_sock);
    }
}