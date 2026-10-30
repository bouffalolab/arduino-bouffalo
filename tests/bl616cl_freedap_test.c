#include "../UNOR4BL616CLBridge/freedap.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect(int value, const char *message)
{
    if (!value) {
        fprintf(stderr, "test failure: %s\n", message);
        exit(1);
    }
}

static int request(const uint8_t *request_data, int request_size, uint8_t *response)
{
    memset(response, 0, 64U);
    return dap_process_request((uint8_t *)request_data, request_size, response, 64);
}

int main(void)
{
    uint8_t response[64];
    int response_size;

    dap_init();

    {
        const uint8_t request_data[] = { 0x00U, 0xf0U };
        response_size = request(request_data, sizeof(request_data), response);
        expect(response_size == 3, "capabilities response length");
        expect(response[0] == 0x00U && response[1] == 1U && response[2] == 1U,
            "SWD capabilities response");
    }

    {
        const uint8_t request_data[] = { 0x00U, 0xffU };
        response_size = request(request_data, sizeof(request_data), response);
        expect(response_size == 4, "packet-size response length");
        expect(
            response[0] == 0x00U &&
            response[1] == 2U &&
            response[2] == 64U &&
            response[3] == 0U,
            "64-byte packet size response");
    }

    {
        const uint8_t request_data[] = { 0x02U, 0x00U };
        response_size = request(request_data, sizeof(request_data), response);
        expect(response_size == 2, "connect response length");
        expect(response[0] == 0x02U && response[1] == 1U, "SWD connect response");
    }

    {
        const uint8_t request_data[] = { 0x03U };
        response_size = request(request_data, sizeof(request_data), response);
        expect(response_size == 2, "disconnect response length");
        expect(response[0] == 0x03U && response[1] == 0U, "disconnect response");
    }

    {
        const uint8_t request_data[] = { 0x7fU };
        response_size = request(request_data, sizeof(request_data), response);
        expect(response_size == 1, "unknown response length");
        expect(response[0] == 0xffU, "unknown command response");
    }

    return 0;
}
