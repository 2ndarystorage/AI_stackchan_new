#ifndef _SCHEDULER_H
#define _SCHEDULER_H

#include <Arduino.h>

#define MAX_SCHED_NUM   (50)

typedef enum e_sched_type
{
    SCHED_EVERY_DAY,
    SCHED_EVERY_HOUR,
    SCHED_REMINDER,
    SCHED_INTERVAL_MINUTE
} SCHED_TYPE;

// 発火可否ゲート（任意）：登録した関数が false を返す間は発火を延期／スキップする。
// nullptr（未登録）の場合は常に発火可。main.cpp 側の scheduler_ready() を登録する想定。
extern void scheduler_set_ready_func(bool (*func)(void));

class ScheduleBase{
protected:
    SCHED_TYPE sched_type;      //子クラスの判別用（本当はtypeinfoを使いたいがビルドに失敗した）
    struct tm prev_time;
    void (*callback)(void);

public:
    bool destroy;
    ScheduleBase();
    virtual void run(struct tm now_time);
    SCHED_TYPE get_sched_type(){ return sched_type; };

};


class ScheduleEveryDay: public ScheduleBase{
private:
    int sched_hour;
    int sched_min;
    int is_executed;
    uint32_t due_since_ms;   // 発火時刻を過ぎてから混雑で待たされている開始時刻（0=未到来）
public:
    // 混雑時に発火を延期する上限（これを超えたらその日はスキップ）。既定5分
    uint32_t max_defer_ms;
    ScheduleEveryDay(int hour, int min, void (*func)(void));
    void run(struct tm now_time);
};

class ScheduleEveryHour: public ScheduleBase{
private:
    int start_hour;
    int end_hour;
public:
    ScheduleEveryHour(void (*func)(void), int _start_hour, int _end_hour);
    void run (struct tm now_time);
};


// 一回限りのスケジュール（Ex ではリマインダー用途。ここでは汎用コールバック版に変更）
class ScheduleReminder: public ScheduleBase{
private:
    int sched_hour;
    int sched_min;
    int is_executed;
    uint32_t due_since_ms;
public:
    // 混雑時に発火を延期する上限（これを超えたら破棄）。既定5分
    uint32_t max_defer_ms;
    ScheduleReminder(int hour, int min, void (*func)(void));
    void run(struct tm now_time);
    String get_time_string();
};


class ScheduleIntervalMinute: public ScheduleBase{
private:
    int interval_min;
    int start_hour;
    int end_hour;
    int is_executed;
    String remind_text;
public:
    ScheduleIntervalMinute(int _interval_min, void (*func)(void), int _start_hour, int _end_hour);
    void run(struct tm now_time);
};


extern void add_schedule(ScheduleBase* schedule);
extern ScheduleBase* get_schedule(int idx);
extern void run_schedule(void);



#endif
