// スケジューラ（ronron-gh/AI_StackChan_Ex の Scheduler を移植）
// Ex からの変更点：
//  - Robot.h / PlayMP3 / FunctionCall 依存を排除し、ScheduleReminder も汎用コールバック化した
//  - 発火可否ゲート（scheduler_set_ready_func）を追加。発話中などは発火しない
//  - ScheduleEveryDay / ScheduleReminder は混雑時、max_defer_ms まで発火を延期する
//  - ScheduleEveryHour は混雑時はその回をスキップ、ScheduleIntervalMinute は次回に持ち越す
#include <Arduino.h>
#include <time.h>
#include "Scheduler.h"

ScheduleBase* scheduleList[MAX_SCHED_NUM] = {nullptr};

static bool (*s_ready_func)(void) = nullptr;

void scheduler_set_ready_func(bool (*func)(void))
{
    s_ready_func = func;
}

// 発火してよい状態か（ゲート未登録なら常時可）
static bool sched_ready(void)
{
    return (s_ready_func == nullptr) || s_ready_func();
}

void add_schedule(ScheduleBase* schedule)
{
    for(int i=0; i<MAX_SCHED_NUM; i++){
        if(scheduleList[i]==nullptr){
            scheduleList[i] = schedule;
            Serial.printf("add_schedule: %d\n",i);
            break;
        }
    }
}

ScheduleBase* get_schedule(int idx)
{
    if(idx > MAX_SCHED_NUM){
        return nullptr;
    }
    return scheduleList[idx];
}

void run_schedule(void)
{
    struct tm now_time;

    if (!getLocalTime(&now_time)) {
        Serial.printf("failed to get time\n");
        return;
    }

    for(int i=0; i<MAX_SCHED_NUM; i++){
        if(scheduleList[i]!=nullptr){
            ScheduleBase* schedule = scheduleList[i];
            schedule->run(now_time);

            if(schedule->destroy){
                Serial.printf("destroy schedule: %d\n",i);
                delete schedule;
                scheduleList[i] = nullptr;
            }

        }

    }
}



ScheduleBase::ScheduleBase()
{
    destroy = false;
}

void ScheduleBase::run(struct tm now_time)
{
    (void)now_time;
}

ScheduleEveryDay::ScheduleEveryDay(int hour, int min, void (*func)(void))
{
    struct tm now_time = {};

    sched_type = SCHED_EVERY_DAY;
    sched_hour = hour;
    sched_min = min;
    callback = func;
    due_since_ms = 0;
    max_defer_ms = 5 * 60 * 1000UL;

    //If the time has already passed, set the status to executed.
    if (getLocalTime(&now_time)) {
        int sched = sched_hour * 60 + sched_min;
        int now = now_time.tm_hour * 60 + now_time.tm_min;

        if(now >= sched){
            is_executed = true;
        }else{
            is_executed = false;
        }

        prev_time = now_time;
    }
    else{
        Serial.printf("failed to get time\n");
        is_executed = false;
        prev_time = now_time;
    }

}

void ScheduleEveryDay::run(struct tm now_time)
{
    int sched = sched_hour * 60 + sched_min;
    int now = now_time.tm_hour * 60 + now_time.tm_min;

    if(now >= sched){
        if(!is_executed){
            if(sched_ready()){
                Serial.printf("Schedule::run callback()\n");
                callback();
                is_executed = true;
                due_since_ms = 0;
            }else{
                // 混雑中は発火を延期し、上限を超えたらその日はスキップする
                if(due_since_ms == 0){
                    due_since_ms = millis();
                }
                if(millis() - due_since_ms > max_defer_ms){
                    Serial.printf("Schedule::run skipped (busy too long)\n");
                    is_executed = true;
                    due_since_ms = 0;
                }
            }
        }
    }

    // Reset schedule when date is updated.
    if(prev_time.tm_hour == 23 && now_time.tm_hour == 0){
        Serial.printf("Reset schedule\n");
        is_executed = false;
        due_since_ms = 0;
    }
    prev_time = now_time;
}



ScheduleEveryHour::ScheduleEveryHour(void (*func)(void), int _start_hour, int _end_hour)
{
    struct tm now_time = {};

    sched_type = SCHED_EVERY_HOUR;
    start_hour = _start_hour;
    end_hour = _end_hour;
    callback = func;

    getLocalTime(&now_time);
    prev_time = now_time;
}

void ScheduleEveryHour::run(struct tm now_time)
{
    if(start_hour <= now_time.tm_hour && now_time.tm_hour <= end_hour){
        if(now_time.tm_hour != prev_time.tm_hour){
            if(sched_ready()){
                Serial.printf("ScheduleEveryHour::run callback()\n");
                callback();
            }else{
                // 時報は混雑時はスキップする（prev_time は進める）
                Serial.printf("ScheduleEveryHour::run skipped (busy)\n");
            }
        }
    }
    prev_time = now_time;
}



ScheduleReminder::ScheduleReminder(int hour, int min, void (*func)(void))
{
    struct tm now_time = {};

    sched_type = SCHED_REMINDER;
    sched_hour = hour;
    sched_min = min;
    callback = func;
    due_since_ms = 0;
    max_defer_ms = 5 * 60 * 1000UL;

    //If the time has already passed, set the status to executed.
    if (getLocalTime(&now_time)) {
        int sched = sched_hour * 60 + sched_min;
        int now = now_time.tm_hour * 60 + now_time.tm_min;

        if(now >= sched){
            is_executed = true;
            destroy = true;
        }else{
            is_executed = false;
        }
    }
    else{
        Serial.printf("failed to get time\n");
        is_executed = false;
    }

}

void ScheduleReminder::run(struct tm now_time)
{
    int sched = sched_hour * 60 + sched_min;
    int now = now_time.tm_hour * 60 + now_time.tm_min;

    if(now >= sched){
        if(!is_executed){
            if(sched_ready()){
                Serial.printf("ScheduleReminder::run callback()\n");
                callback();
                is_executed = true;
                destroy = true;
            }else{
                if(due_since_ms == 0){
                    due_since_ms = millis();
                }
                if(millis() - due_since_ms > max_defer_ms){
                    Serial.printf("ScheduleReminder::run skipped (busy too long)\n");
                    is_executed = true;
                    destroy = true;
                }
            }

        }
    }


}

String ScheduleReminder::get_time_string()
{
    return String(sched_hour) + String(":") + String(sched_min);
}

ScheduleIntervalMinute::ScheduleIntervalMinute(int _interval_min, void (*func)(void), int _start_hour, int _end_hour)
{
    struct tm now_time = {};

    sched_type = SCHED_INTERVAL_MINUTE;
    interval_min = _interval_min;
    start_hour = _start_hour;
    end_hour = _end_hour;
    callback = func;

    getLocalTime(&now_time);
    prev_time = now_time;

}

void ScheduleIntervalMinute::run(struct tm now_time)
{
    if(start_hour <= now_time.tm_hour && now_time.tm_hour <= end_hour){
        int diff = now_time.tm_min - prev_time.tm_min;
        if(diff < 0){
            diff = now_time.tm_min + (60 - prev_time.tm_min);
        }

        if(diff >= interval_min){
            if(sched_ready()){
                Serial.printf("ScheduleIntervalMinute::run callback()\n");
                callback();
                prev_time = now_time;
            }
            // 混雑時は prev_time を進めず、空き次第発火する
        }

    }
}
