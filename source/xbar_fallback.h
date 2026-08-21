#pragma once
static inline bool xbar_fallback_proprels_available() {
    if (!g_app.gpuHandle) return false;
    NvApiFunc getFunc = (NvApiFunc)nvapi_qi(0xCBFF71D0u);
    if (!getFunc) return false;
    unsigned char buf[0x1000] = {};
    unsigned int ver = 0x0001075Cu;
    memcpy(buf, &ver, sizeof(ver));
    if (getFunc(g_app.gpuHandle, buf) != 0) return false;
    unsigned int raw = 0;
    memcpy(&raw, buf + 0x2C, sizeof(raw));
    float ratio = (float)raw / 65536.0f;
    return ratio > 0.4f && ratio < 3.0f;
}
