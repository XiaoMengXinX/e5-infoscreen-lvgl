#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "ui.h"

static double bezier_reference(double t, double x1, double y1, double x2, double y2)
{
    double lo = 0, hi = 1, u = t;
    for (int i = 0; i < 50; i++) {
        u = (lo + hi) / 2;
        double x = 3 * (1 - u) * (1 - u) * u * x1 + 3 * (1 - u) * u * u * x2 + u * u * u;
        if (x < t)
            lo = u;
        else
            hi = u;
    }
    return 3 * (1 - u) * (1 - u) * u * y1 + 3 * (1 - u) * u * u * y2 + u * u * u;
}

int main(void)
{
    const float curves[][4] = {{.3f, 0, 0, 1}, {.22f, .75f, .2f, 1}, {.25f, .1f, .25f, 1}};
    for (unsigned c = 0; c < sizeof curves / sizeof *curves; c++) {
        const float *v = curves[c];
        assert(ui_bezier(0, v[0], v[1], v[2], v[3]) == 0);
        assert(ui_bezier(1, v[0], v[1], v[2], v[3]) == 1);
        for (int i = 1; i < 1000; i++) {
            float t = i / 1000.0f;
            assert(fabs(ui_bezier(t, v[0], v[1], v[2], v[3]) -
                        bezier_reference(t, v[0], v[1], v[2], v[3])) < .00002);
        }
    }
    for (int s = 0; s <= 30; s++) {
        float slope = s / 10.0f, previous = 0;
        assert(ui_release_progress(0, slope) == 0);
        assert(fabsf(ui_release_progress(1, slope) - 1) < .000001f);
        for (int i = 1; i <= 1000; i++) {
            float y = ui_release_progress(i / 1000.0f, slope);
            assert(y >= previous - .000001f && y <= 1.000001f);
            previous = y;
        }
        assert(fabsf(ui_release_progress(.0001f, slope) / .0001f - slope) < .001f);
        assert(fabsf((1 - ui_release_progress(.9999f, slope)) / .0001f) < .003f);
    }
    for (int direction = -1; direction <= 1; direction += 2) {
        float slope = 1.5f;
        lv_anim_t a = {.duration = 200, .start_value = 320 * direction,
                       .end_value = -320 * direction, .user_data = &slope};
        int32_t previous = a.start_value;
        for (int i = 0; i <= 200; i++) {
            a.act_time = i;
            int32_t value = ui_ease_release(&a);
            assert((value - previous) * direction <= 0);
            previous = value;
        }
        assert(previous == a.end_value);
    }
    for (int d = 0; d < 3; d++) {
        float damping = (float[]){.68f, .8f, 1}[d];
        float expected_x = 0, expected_v = 80;
        ui_spring(&expected_x, &expected_v, 100, .36f, damping, .5f);
        const int rates[] = {30, 60, 90, 120};
        for (unsigned r = 0; r < sizeof rates / sizeof *rates; r++) {
            int hz = rates[r];
            float x = 0, v = 80;
            for (int i = 0; i < hz / 2; i++)
                ui_spring(&x, &v, 100, .36f, damping, 1.0f / hz);
            assert(fabsf(x - expected_x) < .001f && fabsf(v - expected_v) < .01f);
        }
        float x = 0, v = 80;
        ui_spring(&x, &v, 100, .36f, damping, .07f);
        ui_spring(&x, &v, 100, .36f, damping, .43f);
        assert(fabsf(x - expected_x) < .001f && fabsf(v - expected_v) < .01f);
        ui_spring(&x, &v, 100, .36f, damping, 10);
        assert(isfinite(x) && isfinite(v) && fabsf(x - 100) < .001f);
    }
    puts("Motion: Bezier accuracy, release velocity, monotonicity and frame-independent springs passed");
}
