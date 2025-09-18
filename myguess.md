You should alway know:
ESP_ERR_HTTPD_CONNECTION_CLOSED 在 ESP-IDF 里没有这个定义。
用 errno 判断是不是对端断开（ECONNRESET / ENOTCONN / EPIPE / ESHUTDOWN）；
EAGAIN / EWOULDBLOCK 就忽略；
for example:
if (ret != ESP_OK) {
    int err = errno;
    if (err == ECONNRESET || err == ENOTCONN || err == EPIPE || err == ESHUTDOWN) {
        ESP_LOGI(TAG, "Client disconnected, fd=%d", fd);
        // Find and remove client from list
        if (xSemaphoreTake(client_mutex, portMAX_DELAY)) {
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (client_slots[i].fd == fd) {
                    client_slots[i].active = false;
                    break;
                }
            }
            xSemaphoreGive(client_mutex);
        }
    } else if (err == EAGAIN || err == EWOULDBLOCK) {
        // 暂时无数据，不算错误
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "httpd_ws_recv_frame failed (ret=%d, errno=%d) for fd=%d", ret, err, fd);
    }
}
