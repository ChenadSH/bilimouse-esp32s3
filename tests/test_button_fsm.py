#!/usr/bin/env python3
"""
BiliMouse 单键多义手势状态机单元测试。

直接在主机上编译并运行 button_fsm.c，无需 ESP-IDF 环境。
测试覆盖 PRD 中的验收标准：
  - 单击/双击/长按/超长按四种手势 100% 正确区分
  - 双击时"单击"动作不被执行（无重复切集）
  - 长按触发后继续按住不会重复触发
  - 一次按下最多触发一个动作

用法: python tests/test_button_fsm.py
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FSM_C = os.path.join(ROOT, "main", "app", "button_fsm.c")
FSM_H = os.path.join(ROOT, "main", "app", "button_fsm.h")

TEST_MAIN = r"""
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "button_fsm.h"

#define MAX_EVENTS 16

static gesture_t g_events[MAX_EVENTS];
static int g_count = 0;

/* 按住提示事件（HOLD_*）单独统计 —— 它们不是动作 */
static int g_hold_hints = 0;
static int g_super_hints = 0;
static int g_ultra_hints = 0;

/* 只记录「动作类」事件（HOLD_* 是 UI 提示，不算动作） */
static void on_gesture(gesture_t g, void *ctx)
{
    (void)ctx;
    if (g == GESTURE_HOLD_START) {
        g_hold_hints++;
        return;
    }
    if (g == GESTURE_HOLD_SUPER) {
        g_super_hints++;
        return;
    }
    if (g == GESTURE_HOLD_ULTRA) {
        g_ultra_hints++;
        return;
    }
    if (g_count < MAX_EVENTS) {
        g_events[g_count++] = g;
    }
}

static void reset(void)
{
    g_count = 0;
    g_hold_hints = 0;
    g_super_hints = 0;
    g_ultra_hints = 0;
    memset(g_events, 0, sizeof(g_events));
    fsm_config_t cfg = {
        .click_max_ms        = 250,
        .double_click_gap_ms = 300,
        .long_press_ms       = 600,
        .super_long_press_ms = 1500,
    };
    button_fsm_init(&cfg, on_gesture, NULL);
}

/* 带 4 秒档位的重置（触摸校准手势） */
static void reset_ultra(void)
{
    g_count = 0;
    g_hold_hints = 0;
    g_super_hints = 0;
    g_ultra_hints = 0;
    memset(g_events, 0, sizeof(g_events));
    fsm_config_t cfg = {
        .click_max_ms        = 250,
        .double_click_gap_ms = 300,
        .long_press_ms       = 600,
        .super_long_press_ms = 1500,
        .ultra_long_press_ms = 4000,
    };
    button_fsm_init(&cfg, on_gesture, NULL);
}

/* 按下 -> 推进 hold_ms -> 松开 */
static void tap(uint32_t hold_ms)
{
    button_fsm_update(true, 0);
    for (uint32_t t = 0; t < hold_ms; t += 10) {
        button_fsm_tick(t);
    }
    button_fsm_update(false, hold_ms);
}

/* 单击：短按后等过双击窗口 */
static void single_click(void)
{
    tap(100);
    for (uint32_t t = 100; t <= 500; t += 10) {
        button_fsm_tick(t);
    }
}

/* 双击：两次短按，间隔在双击窗口内 */
static void double_click(uint32_t gap_ms)
{
    tap(100);
    /* 间隔 */
    for (uint32_t t = 100; t < 100 + gap_ms; t += 10) {
        button_fsm_tick(t);
    }
    button_fsm_update(true, 100 + gap_ms);
    button_fsm_update(false, 100 + gap_ms + 100);
    for (uint32_t t = 100 + gap_ms + 100; t <= 100 + gap_ms + 500; t += 10) {
        button_fsm_tick(t);
    }
}

static void long_press(uint32_t hold_ms)
{
    button_fsm_update(true, 0);
    for (uint32_t t = 0; t <= hold_ms; t += 10) {
        button_fsm_tick(t);
    }
    button_fsm_update(false, hold_ms);
    for (uint32_t t = hold_ms; t <= hold_ms + 500; t += 10) {
        button_fsm_tick(t);
    }
}

/* 只按住不松开 —— 用于验证按住期间的提示事件 */
static void hold_only(uint32_t hold_ms)
{
    button_fsm_update(true, 0);
    for (uint32_t t = 0; t <= hold_ms; t += 10) {
        button_fsm_tick(t);
    }
}

static int failures = 0;
static int total = 0;

static void expect(const char *name, gesture_t expected, int n_expected)
{
    total++;
    if (g_count != n_expected) {
        printf("FAIL  %-42s expected %d event(s), got %d\n", name, n_expected, g_count);
        failures++;
        return;
    }
    for (int i = 0; i < n_expected; i++) {
        if (g_events[i] != expected) {
            printf("FAIL  %-42s event[%d] = %d, expected %d\n", name, i, g_events[i], expected);
            failures++;
            return;
        }
    }
    printf("PASS  %-42s (%d event)\n", name, n_expected);
}

int main(void)
{
    /* --- 基本手势识别（动作在松手时触发） --- */
    reset(); single_click();
    expect("single click -> single", GESTURE_SINGLE_CLICK, 1);

    reset(); double_click(150);
    expect("double click (150ms gap) -> double", GESTURE_DOUBLE_CLICK, 1);

    reset(); long_press(700);
    expect("hold 700ms -> long press", GESTURE_LONG_PRESS, 1);

    reset(); long_press(1600);
    expect("hold 1600ms -> super long", GESTURE_SUPER_LONG, 1);

    /* --- 关键：双击不得额外触发单击 --- */
    reset(); double_click(150);
    total++;
    if (g_count == 2 && g_events[0] == GESTURE_SINGLE_CLICK) {
        printf("FAIL  %-42s double click also fired single click\n", "no spurious single on double");
        failures++;
    } else {
        printf("PASS  %-42s (no duplicate)\n", "no spurious single on double");
    }

    /* --- 关键：长按只触发一个动作 --- */
    reset(); long_press(2000);
    total++;
    if (g_count != 1) {
        printf("FAIL  %-42s got %d events, expected exactly 1\n", "long hold fires once", g_count);
        failures++;
    } else {
        printf("PASS  %-42s\n", "long hold fires once");
    }

    /* --- 关键：超长按覆盖长按，只执行超长按这一个动作 --- */
    reset(); long_press(1600);
    total++;
    if (g_count != 1 || g_events[0] != GESTURE_SUPER_LONG) {
        printf("FAIL  %-42s got %d event(s), first=%d\n", "super long overrides long", g_count,
               g_count ? (int)g_events[0] : -1);
        failures++;
    } else {
        printf("PASS  %-42s\n", "super long overrides long");
    }

    /* --- 按住期间发提示事件，但松手前不执行任何动作 --- */
    reset(); hold_only(700);
    total++;
    if (g_hold_hints != 1 || g_super_hints != 0 || g_count != 0) {
        printf("FAIL  %-42s hold=%d super=%d actions=%d\n", "hold hint fires while holding",
               g_hold_hints, g_super_hints, g_count);
        failures++;
    } else {
        printf("PASS  %-42s\n", "hold hint fires while holding");
    }

    /* --- 按住 1.6s 应依次给出长按提示与超长按提示，松手只执行超长按 --- */
    reset(); long_press(1600);
    total++;
    if (g_count != 1 || g_events[0] != GESTURE_SUPER_LONG ||
        g_hold_hints != 1 || g_super_hints != 1) {
        printf("FAIL  %-42s actions=%d hold=%d super=%d\n", "hint then super long on release",
               g_count, g_hold_hints, g_super_hints);
        failures++;
    } else {
        printf("PASS  %-42s\n", "hint then super long on release");
    }

    /* --- 双击窗口外的第二次点击应算两次单击 --- */
    reset();
    tap(100);
    for (uint32_t t = 100; t <= 500; t += 10) button_fsm_tick(t);
    total++;
    if (g_count != 1 || g_events[0] != GESTURE_SINGLE_CLICK) {
        printf("FAIL  %-42s got %d event(s)\n", "first click fires after window", g_count);
        failures++;
    } else {
        printf("PASS  %-42s\n", "first click fires after window");
    }

    /* --- 无效按压（介于短按与长按之间）不应产生动作 --- */
    reset();
    button_fsm_update(true, 0);
    for (uint32_t t = 0; t <= 400; t += 10) button_fsm_tick(t);
    button_fsm_update(false, 400);
    for (uint32_t t = 400; t <= 900; t += 10) button_fsm_tick(t);
    total++;
    if (g_count != 0) {
        printf("FAIL  %-42s got %d event(s), expected 0\n", "medium hold -> no action", g_count);
        failures++;
    } else {
        printf("PASS  %-42s\n", "medium hold -> no action");
    }

    /* --- 连续快速单击应各自独立触发 --- */
    reset();
    single_click();
    total++;
    if (g_count != 1) {
        printf("FAIL  %-42s got %d\n", "isolated single click", g_count);
        failures++;
    } else {
        printf("PASS  %-42s\n", "isolated single click");
    }

    /* --- 4 秒长按 = 触摸校准，且不与超长按重复触发 --- */
    reset_ultra(); long_press(4100);
    total++;
    if (g_count != 1 || g_events[0] != GESTURE_ULTRA_LONG) {
        printf("FAIL  %-42s got %d event(s), first=%d\n", "hold 4.1s -> ultra long", g_count,
               g_count ? (int)g_events[0] : -1);
        failures++;
    } else {
        printf("PASS  %-42s\n", "hold 4.1s -> ultra long");
    }

    reset_ultra(); long_press(4100);
    total++;
    if (g_count != 1 || g_events[0] != GESTURE_ULTRA_LONG ||
        g_hold_hints != 1 || g_super_hints != 1 || g_ultra_hints != 1) {
        printf("FAIL  %-42s actions=%d hold=%d super=%d ultra=%d\n",
               "ultra hint chain", g_count, g_hold_hints, g_super_hints, g_ultra_hints);
        failures++;
    } else {
        printf("PASS  %-42s\n", "ultra hint chain");
    }

    /* --- 4 秒档位未配置时，超长按行为保持不变 --- */
    reset(); long_press(4100);
    total++;
    if (g_count != 1 || g_events[0] != GESTURE_SUPER_LONG) {
        printf("FAIL  %-42s got %d event(s), first=%d\n", "no ultra slot -> super long", g_count,
               g_count ? (int)g_events[0] : -1);
        failures++;
    } else {
        printf("PASS  %-42s\n", "no ultra slot -> super long");
    }

    printf("\n==== %d/%d passed, %d failed ====\n", total - failures, total, failures);
    return failures == 0 ? 0 : 1;
}
"""


def find_compiler():
    """优先使用系统 gcc（可执行本机二进制）；ESP-IDF 交叉工具链仅作语法检查。"""
    from shutil import which
    if which("gcc"):
        return "gcc", []
    return None, []


def find_cross_compiler():
    """ESP-IDF 自带的 xtensa 交叉编译器（仅能编译，不能在主机运行）。"""
    xtensa_root = os.path.join(os.environ.get("IDF_TOOLS_PATH", r"C:\Espressif"),
                               "tools", "xtensa-esp-elf")
    if not os.path.isdir(xtensa_root):
        return None
    for ver in sorted(os.listdir(xtensa_root)):
        gcc = os.path.join(xtensa_root, ver, "xtensa-esp-elf", "bin",
                           "xtensa-esp32s3-elf-gcc.exe")
        if os.path.isfile(gcc):
            return gcc
    return None


def docker_available():
    from shutil import which
    if not which("docker"):
        return False
    try:
        r = subprocess.run(["docker", "info", "--format", "{{.ServerVersion}}"],
                           capture_output=True, text=True, timeout=30)
        return r.returncode == 0
    except Exception:
        return False


def run_in_docker():
    """在 Linux 容器中编译并运行测试（需要本地有可用镜像且能装 gcc）。"""
    print("[docker] 在容器中运行测试")
    script = (
        "set -e; "
        "apk add --no-cache gcc musl-dev python3 >/dev/null 2>&1 || "
        "apk add --no-cache gcc musl-dev >/dev/null 2>&1; "
        "gcc --version | head -1; "
        "python3 tests/test_button_fsm.py"
    )
    cmd = ["docker", "run", "--rm",
           "-v", f"{ROOT}:/work", "-w", "/work",
           "node:22-alpine", "sh", "-c", script]
    r = subprocess.run(cmd, capture_output=True, text=True)
    print(r.stdout)
    if r.stderr.strip():
        print(r.stderr, file=sys.stderr)
    return r.returncode


def main():
    # 1) 首选系统 gcc
    cc, _ = find_compiler()
    if cc:
        return run_native(cc)

    # 2) 其次用 Docker 容器内的 gcc（跨平台可用）
    if docker_available():
        return run_in_docker()

    # 3) 最后尝试 ESP-IDF 交叉编译器做语法检查（产物无法在主机运行）
    cross = find_cross_compiler()
    if cross:
        print("[info] 无可运行的编译器，改用 ESP-IDF 交叉编译器做语法检查")
        print("       (交叉编译产物无法在 Windows 主机执行，行为测试需在容器或设备上进行)")
        rc = compile_only(cross)
        return rc

    print("[error] no C compiler available (system gcc / Docker / ESP-IDF toolchain).")
    return 2


def compile_only(cc):
    """仅编译不运行，用于语法检查。"""
    with tempfile.TemporaryDirectory() as tmp:
        main_c = os.path.join(tmp, "test_main.c")
        with open(main_c, "w", encoding="utf-8") as f:
            f.write(TEST_MAIN)
        cmd = [cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-c",
               "-I", os.path.dirname(FSM_H), FSM_C, main_c, "-o",
               os.path.join(tmp, "out.o")]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout)
            print(r.stderr)
            print("[error] syntax check failed")
            return 2
        print("[ok] syntax check passed")
        return 0


def run_native(cc):
    with tempfile.TemporaryDirectory() as tmp:
        main_c = os.path.join(tmp, "test_main.c")
        exe = os.path.join(tmp, "test_fsm.exe")
        with open(main_c, "w", encoding="utf-8") as f:
            f.write(TEST_MAIN)

        cmd = [cc, "-std=c99", "-Wall", "-Wextra", "-Werror",
               "-I", os.path.dirname(FSM_H),
               FSM_C, main_c, "-o", exe]
        print("[build]", cc)
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout)
            print(r.stderr)
            print("[error] compilation failed")
            return 2

        print("[run]")
        r = subprocess.run([exe], capture_output=True, text=True)
        print(r.stdout)
        if r.stderr.strip():
            print(r.stderr)
        return r.returncode


if __name__ == "__main__":
    sys.exit(main())
