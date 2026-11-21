/* 平台判断用户讲话，下行按有效 PCM 活动计时。单调毫秒低 32 位，支持回绕。 */
#ifndef SESSION_POLICY_H
#define SESSION_POLICY_H
#include <stdbool.h>
#include <stdint.h>

#define SESSION_SILENCE_TIMEOUT_MS 30000u

static inline bool session_silence_expired(uint32_t now, uint32_t started,
                                         bool user_speaking, uint32_t speech_stopped,
                                         uint32_t output_voice)
{
    /* start 到 stop 期间禁止静默挂断，即使用户讲话超过 30 秒且下行全是静音。
     * stop 后从较晚的停止讲话/有效输出时刻重新算完整 30 秒，不恢复旧倒计时。
     * 尚无讲话事件或输出时从会话开始计时；旧会话时间不能延长本次会话。 */
    if (user_speaking) return false;
    uint32_t quiet_ms = now - started;
    if (speech_stopped && now - speech_stopped < quiet_ms) quiet_ms = now - speech_stopped;
    if (output_voice && now - output_voice < quiet_ms) quiet_ms = now - output_voice;
    return quiet_ms >= SESSION_SILENCE_TIMEOUT_MS;
}
#endif
