// calibration.h — 游戏内转一圈校准的纯逻辑（不含 Win32）：Idle → Run1 → Between → Run2 → Done(瞬时) → Idle 状态机、有符号 Σdx 计数、合法性 / 一致性 / 超时 / 离开游戏规则、提示文案、结果换算与按 exe 查表。
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace mc {

// turn-calibration spec 的常量。
inline constexpr long long kCalibMinCounts = 800;        // 单次 counts_360 合法下限（含）
inline constexpr long long kCalibMaxCounts = 400000;     // 单次 counts_360 合法上限（含）
inline constexpr double kCalibStateTimeoutS = 60.0;      // Run1 / Between / Run2 各自的停留上限

// toast 文案（turn-calibration spec 原文）在 i18n 文字表里：CalibNotReady、CalibBadCount、CalibInconsistent、
// CalibTimeout、CalibLeftGame、CalibDone，提示为 CalibRun1 / CalibBetween / CalibRun2（按当前界面语言）。

// Done 只在 on_hotkey 内部经过，state() 在外部永远看不到它。
enum class CalibState { Idle, Run1, Between, Run2, Done };

// 状态的英文名（日志 "calib state=Idle->Run1" 用）。
const char* calib_state_name(CalibState s);

// 调用时刻的前台情况（app 由最新的 GameContext 填写）。
struct CalibContext {
    bool in_game = false;
    bool cursor_locked = false;
    std::wstring exe;   // 前台 exe（比较时不区分大小写）
};

// 结算成功时要保存的结果。
struct CalibSave {
    std::wstring exe;                // 校准对象（小写）
    double counts_per_degree = 0;    // ((c1 + c2) / 2) / 360
    long long c1 = 0, c2 = 0;        // 两次的 counts_360
};

// 一次调用产生的全部副作用，由 app 执行（toast 交给 overlay、save 写入配置并保存）。
struct CalibEffect {
    std::wstring toast;              // 空串 = 无
    std::optional<CalibSave> save;   // 有值 = 结算成功
    bool empty() const { return toast.empty() && !save.has_value(); }
};

// 校准状态机。只有 on_hotkey 能推进状态（没有任何按键 / Esc 入口）；update 只做超时与离开游戏的取消。
class Calibration {
public:
    // calibrate 热键：Idle 开始第 1 次（需 in_game && cursor_locked）、Run1 结束第 1 次、Between 开始第 2 次、
    // Run2 结束第 2 次并结算。非 Idle 时先检查"离开游戏"，再检查超时，任一成立则取消并返回对应 toast。
    CalibEffect on_hotkey(double now_s, const CalibContext& ctx);
    // 一个（或一批）相对鼠标事件的水平位移之和；只在 Run1 / Run2 中带符号累加。
    void on_mouse_dx(long long dx);
    // 每个 tick 调用：非 Idle 时，前台 exe 与校准对象不同 → 取消（"已离开游戏，校准取消"）；
    // 在当前状态停留超过 60 s → 取消（"校准超时，已取消"）。
    CalibEffect update(double now_s, const CalibContext& ctx);
    // 静默取消（重新加载配置、删除正在校准的条目时用），不产生 toast。
    void cancel();

    CalibState state() const { return state_; }
    bool active() const { return state_ != CalibState::Idle; }
    // 当前记录的 |Σdx|（Run1 / Run2 中有意义）。
    long long current_abs_count() const;
    // 第 1 次的结果（Between / Run2 中有意义）。
    long long first_count() const { return c1_; }
    // 校准对象 exe（小写；Idle 时为空）。
    const std::wstring& target_exe() const { return target_; }
    // 进入当前状态的时刻。
    double entered_at() const { return t_enter_; }
    // overlay 的多行提示（\n 分行）；Idle 时返回空串。hotkey_text 为 calibrate 当前绑定的规范字符串。
    std::wstring message(std::wstring_view hotkey_text) const;

private:
    // 非 Idle 时的取消检查（离开游戏优先于超时）；没有取消时返回空效果。
    CalibEffect check_abort(double now_s, const CalibContext& ctx);
    void enter(CalibState s, double now_s);
    void reset();

    CalibState state_ = CalibState::Idle;
    std::wstring target_;
    long long sum_dx_ = 0;
    long long c1_ = 0;
    double t_enter_ = 0;
};

// counts_360 ∈ [800, 400000]。
bool calib_count_valid(long long counts_360);
// 以两次平均为基准：|c1 − c2| / ((c1 + c2) / 2) ≤ 0.15（整数运算，边界精确）。
bool calib_runs_consistent(long long c1, long long c2);
// ((c1 + c2) / 2) / 360。
double calib_counts_per_degree(long long c1, long long c2);
// "校准完成：每度 X 计数"，X 保留 1 位小数（不附等效灵敏度：按 Source 系视角系数的换算对其他游戏没有意义）。
std::wstring calib_result_toast(double counts_per_degree);
// [calibration] 条目的值是否合法：有限，且 cpd × 360 ∈ [800, 400000]。
bool calib_cpd_valid(double counts_per_degree);
// [calibration] 条目值的写法：固定 3 位小数、'.' 为小数点（与区域设置无关）。四舍五入后若落到合法范围之外
// （只可能发生在下界 800 / 360 附近），改为向范围内取整，保证写出的值读回来仍然合法。
std::string calib_format_cpd(double counts_per_degree);
// [calibration] 条目的日期写法 YYYY-MM-DD（变更 4：结算时写入本地日期）。
std::wstring calib_format_date(int year, int month, int day);
// 按 exe 查校准值（不区分大小写；table 的键为小写 exe）；查不到或值不合法时返回 fallback。
double counts_per_degree_for(std::wstring_view exe, const std::map<std::wstring, double>& table, double fallback);

}  // namespace mc
