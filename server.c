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

#define _GNU_SOURCE                  // 啟用 GNU 擴充功能（例如 fopen64）
#define _LARGEFILE64_SOURCE         // 支援 64-bit 檔案操作（>2GB）
#define _FILE_OFFSET_BITS 64        // 將 off_t 強制為 64-bit

#include <stdio.h>                  // 標準輸入輸出
#include <stdlib.h>                 // exit, malloc 等
#include <string.h>                 // 字串處理
#include <unistd.h>                 // close, usleep
#include <arpa/inet.h>              // 網路位址轉換
#include <sys/socket.h>             // socket API
#include <sys/stat.h>               // stat() 檔案資訊
#include <sys/wait.h>               // waitpid()
#include <signal.h>                 // signal 處理
#include <errno.h>                  // 錯誤碼 errno

#define CONTROL_PORT 21             // FTP 控制通道 (Port 21)
#define DATA_PORT 22                // FTP 資料通道 (Port 22)
#define MAX_CLIENTS 3               // 最大同時連線數

volatile sig_atomic_t active_clients = 0; 
// 記錄目前連線人數（volatile 確保 signal handler 可安全存取）

/* ===== SIGCHLD 處理器：回收子行程 ===== */
void sigchld_handler(int s) {
    int saved_errno = errno;        // 保存 errno，避免 waitpid 改變它

    // 非阻塞回收所有已結束的子行程
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        if (active_clients > 0) active_clients--; // 減少活躍 client 數
    }

    errno = saved_errno;            // 還原 errno
}

/* ===== 處理單一 client ===== */
void handle_client(int control_sock, int data_sock) {
    char filename[256] = {0};       // 儲存 client 傳來的檔名
    off_t offset = 0;               // 續傳位置（64-bit）

    /* 接收檔名 */
    ssize_t res = recv(control_sock, filename, sizeof(filename) - 1, 0);

    if (res <= 0) {                 // 接收失敗或斷線
        if (res < 0) perror("recv filename error");
        exit(1);
    }

    printf("[PID %d] 接收檔案要求: %s\n", getpid(), filename);

    /* 檢查是否已有檔案（續傳） */
    struct stat st;
    if (stat(filename, &st) == 0) {
        offset = st.st_size;        // 若存在，設定 offset = 檔案大小
    } else {
        offset = 0;                 // 否則從頭開始
    }

    /* 傳送 offset 給 client */
    if (send(control_sock, &offset, sizeof(offset), 0) < 0) {
        perror("send offset error");
        exit(1);
    }

    /* 開啟檔案（append binary 模式） */
    FILE *fp = fopen64(filename, "ab"); // 支援大檔案
    if (!fp) {
        perror("fopen64 error");
        exit(1);
    }

    char buffer[1024];              // 資料緩衝區
    ssize_t n;

    printf("[PID %d] 開始接收數據 (起始 offset=%lld)\n",
           getpid(), (long long)offset);

    /* 接收資料並寫入檔案 */
    while ((n = recv(data_sock, buffer, sizeof(buffer), 0)) > 0) {

        // 寫入檔案（檢查是否完整寫入）
        if (fwrite(buffer, 1, n, fp) < n) {
            perror("fwrite error");
            break;
        }

        usleep(200000); // 人為延遲，方便觀察多 client 同時傳輸
    }

    if (n < 0) perror("recv data error");

    printf("[PID %d] 傳輸完成: %s\n", getpid(), filename);

    fclose(fp);                     // 關閉檔案
    close(control_sock);            // 關閉 control socket
    close(data_sock);               // 關閉 data socket

    exit(0);                        // 子行程結束
}

/* ===== 主程式 ===== */
int main() {
    int control_sock, data_sock;    // 兩個 socket（控制 + 資料）
    struct sockaddr_in addr;

    /* 註冊 SIGCHLD handler */
    struct sigaction sa;
    sa.sa_handler = sigchld_handler; // 指定 handler
    sigemptyset(&sa.sa_mask);        // 清空 mask
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP; 
    // SA_RESTART：被中斷的系統呼叫自動重啟
    // SA_NOCLDSTOP：忽略子行程暫停訊號

    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction error");
        exit(1);
    }

    /* 建立 control socket */
    if ((control_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("control socket create failed");
        exit(1);
    }

    /* 建立 data socket */
    if ((data_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("data socket create failed");
        exit(1);
    }

    /* 設定 socket 可重用 */
    int opt = 1;
    setsockopt(control_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(data_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* 初始化位址結構 */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; // 接受任何 IP

    /* 綁定 Control Port (21) */
    addr.sin_port = htons(CONTROL_PORT);
    if (bind(control_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind Port 21 failed");
        exit(1);
    }

    if (listen(control_sock, 10) < 0) {
        perror("listen Port 21 failed");
        exit(1);
    }

    /* 綁定 Data Port (22) */
    addr.sin_port = htons(DATA_PORT);
    if (bind(data_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind Port 22 failed");
        exit(1);
    }

    if (listen(data_sock, 10) < 0) {
        perror("listen Port 22 failed");
        exit(1);
    }

    printf("==== FTP Server 啟動 (Listening on 21 & 22) ====\n");

    /* 主迴圈：持續接受 client */
    while (1) {
        struct sockaddr_in c_addr;
        socklen_t len = sizeof(c_addr);

        /* 接受 control 連線 */
        int c_sock = accept(control_sock, (struct sockaddr*)&c_addr, &len);

        if (c_sock < 0) {
            if (errno == EINTR) continue; // 被 signal 中斷就重試
            perror("accept error");
            continue;
        }

        /* 檢查是否超過人數限制 */
        if (active_clients >= MAX_CLIENTS) {
            printf("[拒絕] IP: %s, 已達人數上限 %d\n",
                   inet_ntoa(c_addr.sin_addr), MAX_CLIENTS);
            close(c_sock);
            continue;
        }

        active_clients++;

        printf("[連線] 來自 %s, 目前人數: %d/%d\n",
               inet_ntoa(c_addr.sin_addr),
               active_clients, MAX_CLIENTS);

        pid_t pid = fork(); // 建立子行程

        if (pid < 0) {
            perror("fork failed");
            active_clients--;
            close(c_sock);
        }
        else if (pid == 0) {
            /* 子行程 */

            close(control_sock); // 子行程不用監聽 socket

            struct sockaddr_in d_addr;
            int d_sock;

            /* 等待 data port 並進行 IP 配對 */
            while (1) {
                socklen_t dlen = sizeof(d_addr);

                d_sock = accept(data_sock, (struct sockaddr*)&d_addr, &dlen);

                if (d_sock < 0) {
                    perror("data port accept error");
                    exit(1);
                }

                /* 確保 control 和 data 是同一個 client */
                if (c_addr.sin_addr.s_addr == d_addr.sin_addr.s_addr) {
                    break; // 成功配對
                } else {
                    printf("[警告] 攔截錯誤來源: %s\n",
                           inet_ntoa(d_addr.sin_addr));
                    close(d_sock);
                }
            }

            handle_client(c_sock, d_sock);
        }
        else {
            /* 父行程 */
            close(c_sock); // 交給子行程處理
        }
    }

    return 0;
}
