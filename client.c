/* ============================================
   FTP-like Client (Control:21 / Data:22)

   編譯：
   gcc client.c -o client

   執行：
   ./client <檔名>

   注意：
   Server 必須先開（sudo ./server）

============================================ */

#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define SERVER_IP "127.0.0.1"   // Server IP
#define CONTROL_PORT 21         // 控制通道
#define DATA_PORT 22            // 資料通道
#define BUFFER_SIZE 8192        // buffer 大小

/* ===== 確保 send 完整（避免 TCP 分段） ===== */
ssize_t send_all(int sock, void *buf, size_t len) {
    size_t total = 0;                           // 已送出長度

    while (total < len) {                       // 還沒送完
        ssize_t n = send(sock, (char*)buf + total, len - total, 0);

        if (n <= 0) return n;                   // 發生錯誤

        total += n;                             // 累加已送出
    }

    return total;                               // 回傳總長度
}

int main(int argc, char *argv[]) {

    /* ===== 檢查參數 ===== */
    if (argc < 2) {
        printf("用法: %s <檔名>\n", argv[0]);
        return 1;
    }

    char *filename = argv[1];                   // 要傳的檔名

    int control_sock, data_sock;                // 兩個 socket
    struct sockaddr_in server_addr;             // server 位址

    /* ===== 建立 socket ===== */
    control_sock = socket(AF_INET, SOCK_STREAM, 0); // control socket
    data_sock    = socket(AF_INET, SOCK_STREAM, 0); // data socket

    server_addr.sin_family = AF_INET;           // IPv4
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr); // IP 轉換

    /* =========================================
       ⚠️ 關鍵修正：先建立「兩條連線」
    ========================================= */

    /* ===== 連線 Control Port ===== */
    server_addr.sin_port = htons(CONTROL_PORT);

    if (connect(control_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("control connect error");
        return 1;
    }

    /* ===== 連線 Data Port（很重要🔥） ===== */
    server_addr.sin_port = htons(DATA_PORT);

    if (connect(data_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("data connect error");
        return 1;
    }

    /* =========================================
       控制通道：傳檔名 & 收 offset
    ========================================= */

    /* 傳送檔名 */
    if (send(control_sock, filename, strlen(filename) + 1, 0) < 0) {
        perror("send filename error");
        return 1;
    }

    /* 接收 offset（續傳位置） */
    off_t offset = 0;

    if (recv(control_sock, &offset, sizeof(offset), 0) != sizeof(offset)) {
        perror("recv offset error");
        return 1;
    }

    printf("續傳位置: %lld bytes\n", (long long)offset);

    /* =========================================
       資料通道：傳檔案
    ========================================= */

    FILE *fp = fopen64(filename, "rb");         // 開啟檔案

    if (!fp) {
        perror("file open error");
        return 1;
    }

    /* 跳到續傳位置 */
    fseeko(fp, offset, SEEK_SET);

    char buffer[BUFFER_SIZE];
    size_t n;

    /* 開始傳送檔案 */
    while ((n = fread(buffer, 1, BUFFER_SIZE, fp)) > 0) {

        if (send_all(data_sock, buffer, n) <= 0) {
            perror("send file error");
            break;
        }
    }

    printf("傳送完成\n");

    /* ===== 收尾 ===== */
    fclose(fp);
    close(control_sock);
    close(data_sock);

    return 0;
}
