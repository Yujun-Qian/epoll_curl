#include <assert.h>
#include <curl/curl.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <execinfo.h>

void print_trace(void) {
    char **strings;
    size_t i, size;
    enum Constexpr { MAX_SIZE = 1024 };
    void *array[MAX_SIZE];
    size = backtrace(array, MAX_SIZE);
    strings = backtrace_symbols(array, size);
    for (i = 0; i < size; i++)
        printf("%s\n", strings[i]);
    puts("");
    free(strings);
}

void error(const char *string) {
    perror(string);
    exit(1);
}

int epollFd;
int timeout = -1;

int socketCallback(CURL *easy, curl_socket_t fd, int action, void *u, void *s) {
    struct epoll_event event;
    event.events = 0;
    event.data.fd = fd;

    printf(">>> thread:%d, %s(%3d): enter socketCallback\n", syscall(SYS_gettid), __func__, __LINE__);
    print_trace();

    if (action == CURL_POLL_REMOVE) {
        printf(">>> thread:%d, %s(%3d): removing fd=%d\n", syscall(SYS_gettid), __func__, __LINE__, fd);

        int res = epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, &event);
        if (res == -1 && errno != EBADF)
            error("epoll_ctl(DEL)");
        return 0;
    }

    if (action == CURL_POLL_IN || action == CURL_POLL_INOUT) {
        event.events |= EPOLLIN;
    }
    if (action == CURL_POLL_OUT || action == CURL_POLL_INOUT) {
        event.events |= EPOLLOUT;
    }

    char *actionStr;
    if (action == CURL_POLL_IN) {
        actionStr = "CURL_POLL_IN";
    } else if (action == CURL_POLL_INOUT) {
        actionStr = "CURL_POLL_INOUT";
    } else if (action == CURL_POLL_OUT) {
        actionStr = "CURL_POLL_OUT";
    }
    printf(">>> thread:%d, %s(%3d): adding fd=%d action=%s\n", syscall(SYS_gettid), __func__, __LINE__, fd, actionStr);

    if (event.events != 0) {
        int res = epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &event);
        if (res == -1)
            res = epoll_ctl(epollFd, EPOLL_CTL_MOD, fd, &event);
        if (res == -1)
            error("epoll_ctl(MOD)");
    }

    return 0;
}

int timerCallback(CURLM *multi, long timeout_ms, void *u) {
    printf(">>> thread:%d, %s(%3d): enter timerCallback, timeout: %ld ms\n", syscall(SYS_gettid), __func__, __LINE__, timeout_ms);
    print_trace();
    timeout = timeout_ms;
    return 0;
}

size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    printf(">>> thread:%d, %s(%3d): enter write_callback\n", syscall(SYS_gettid), __func__, __LINE__);
    print_trace();
    //return nmemb;
    return fwrite(ptr, size, nmemb, (FILE *) userdata);
}

/*
size_t write_callback1(char *ptr, size_t size, size_t nmemb, void *userdata) {
    printf(">>> thread:%d, %s(%3d): enter write_callback1\n", syscall(SYS_gettid), __func__, __LINE__);
    //return nmemb;
    return fwrite(ptr, size, nmemb, (FILE *) userdata);
}
*/


// Please note: in this sample, all the callbacks are executed on the same thread
int main(int argc, char **argv) {
    //argv[1] == URL1
    //argv[2] == URL2
    assert(argc >= 2);

    printf("thread:%d, Using %s\n", syscall(SYS_gettid), curl_version());

    FILE *file = fopen("file.html", "w");
    CURL *easy = curl_easy_init();
    curl_easy_setopt(easy, CURLOPT_URL, argv[1]);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, (void *) file);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_callback);

    file = fopen("file1.html", "w");
    CURL *easy1 = curl_easy_init();
    curl_easy_setopt(easy1, CURLOPT_URL, argv[2]);
    curl_easy_setopt(easy1, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(easy1, CURLOPT_WRITEDATA, (void *) file);
    curl_easy_setopt(easy1, CURLOPT_WRITEFUNCTION, write_callback);

    CURLM *multi = curl_multi_init();
    curl_multi_setopt(multi, CURLMOPT_SOCKETFUNCTION, socketCallback);
    curl_multi_setopt(multi, CURLMOPT_TIMERFUNCTION, timerCallback);

    curl_multi_add_handle(multi, easy);
    curl_multi_add_handle(multi, easy1);

    curl_global_init(CURL_GLOBAL_ALL);

    epollFd = epoll_create(1);
    if (epollFd == -1)
        error("epoll_create");

    int running_handles = 1;
    while (running_handles > 0) {
        printf(">>> thread:%d, calling epoll_wait, current timeout is %d\n", syscall(SYS_gettid), timeout);
        struct epoll_event event;
        int res = epoll_wait(epollFd, &event, 1, timeout);
        if (res == -1) {
            error("epoll_wait");
        } else if (res == 0) {
            // The whole process starts here because the timeout has been set to 0 by the timerCallback
            // before call epoll_wait
            printf(">>> thread:%d, %s(%3d): start the whole process\n", syscall(SYS_gettid), __func__, __LINE__);

            // It is also permissible to pass CURL_SOCKET_TIMEOUT to the sockfd parameter
            // in order to initiate the whole process or when a timeout occurs.
            curl_multi_socket_action(multi, CURL_SOCKET_TIMEOUT, 0,
                                     &running_handles);
        } else {
            printf(">>> thread:%d, %s(%3d): or it actually starts here?\n", syscall(SYS_gettid), __func__,
                   __LINE__);
            curl_multi_socket_action(multi, event.data.fd, 0,
                                     &running_handles);
        }

        printf(">>> thread:%d, %s(%3d): running_handles: %d\n\n", syscall(SYS_gettid), __func__, __LINE__,
               running_handles);
    }

    curl_global_cleanup();
    close(epollFd);

    printf(">>> thread:%d, bye bye\n", syscall(SYS_gettid));
    return 0;
}
