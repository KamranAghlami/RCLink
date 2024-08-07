#include "application/application.h"

#include <sys/stat.h>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <sol/sol.hpp>

#include "hardware/display.h"
#include "hardware/wifi.h"
#include "hardware/battery.h"
#include "server/http_server.h"
#include "server/websocket_server.h"

constexpr size_t initial_balls = 25;

struct ball
{
    lv_obj_t *obj_handle;

    struct
    {
        float x;
        float y;
    } position;

    struct
    {
        float x;
        float y;
    } velocity;
};

class rc_link : public application
{
public:
    rc_link() : mp_http_server(std::make_unique<http_server>(80, LV_FS_POSIX_PATH "/web")),
                mp_websocket_server(std::make_unique<websocket_server>(81)),
                m_width(hardware::display::get().width()),
                m_height(hardware::display::get().height()),
                m_group(lv_group_create()),
                m_screen(lv_scr_act())
    {
        m_sol_state.open_libraries();

        struct stat file_stat;

        if (!stat("/scripts/main.lua", &file_stat))
            m_sol_state.script_file("/scripts/main.lua");

        auto websocket_task = [](void *argument)
        {
            auto &server = *static_cast<websocket_server *>(argument);

            while (true)
            {
                tlvcpp::tlv_tree_node node;

                server >> node;

                if (node.data().tag() || node.children().size())
                    server << node;

                vTaskDelay(pdMS_TO_TICKS(100));
            }

            vTaskDelete(nullptr);
        };

        xTaskCreatePinnedToCore(websocket_task, "dispatch_worker", 4U * 1024U, mp_websocket_server.get(), 5, &m_websocket_task, 0);

        lv_indev_t *indev = nullptr;

        while ((indev = lv_indev_get_next(indev)))
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD)
                lv_indev_set_group(indev, m_group);

        lv_group_add_obj(m_group, m_screen);

        auto on_key = [](lv_event_t *e)
        {
            auto app = static_cast<rc_link *>(lv_event_get_user_data(e));
            auto key = lv_event_get_key(e);

            switch (key)
            {
            case LV_KEY_UP:
                app->add_ball();
                break;
            case LV_KEY_DOWN:
                app->remove_ball();
                break;
            case LV_KEY_ENTER:
                app->reset_balls();
                break;
            default:
                break;
            }
        };

        lv_obj_add_event_cb(m_screen, on_key, LV_EVENT_KEY, this);
    }

    ~rc_link()
    {
        while (m_balls.size())
            remove_ball();

        lv_group_del(m_group);

        vTaskDelete(m_websocket_task);
    }

private:
    void on_create() override
    {
        lv_obj_clear_flag(m_screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(m_screen, lv_color_black(), LV_STATE_DEFAULT);

        m_ssid = lv_label_create(lv_layer_top());

        lv_obj_set_style_text_color(m_ssid, lv_color_white(), LV_STATE_DEFAULT);
        lv_obj_align(m_ssid, LV_ALIGN_BOTTOM_LEFT, 4, -94);

        m_ip = lv_label_create(lv_layer_top());

        lv_obj_set_style_text_color(m_ip, lv_color_white(), LV_STATE_DEFAULT);
        lv_obj_align(m_ip, LV_ALIGN_BOTTOM_LEFT, 4, -76);

        m_netmask = lv_label_create(lv_layer_top());

        lv_obj_set_style_text_color(m_netmask, lv_color_white(), LV_STATE_DEFAULT);
        lv_obj_align(m_netmask, LV_ALIGN_BOTTOM_LEFT, 4, -58);

        m_gatway = lv_label_create(lv_layer_top());

        lv_obj_set_style_text_color(m_gatway, lv_color_white(), LV_STATE_DEFAULT);
        lv_obj_align(m_gatway, LV_ALIGN_BOTTOM_LEFT, 4, -40);

        m_battery_voltage = lv_label_create(lv_layer_top());

        lv_obj_set_style_text_color(m_battery_voltage, lv_color_white(), LV_STATE_DEFAULT);
        lv_obj_align(m_battery_voltage, LV_ALIGN_BOTTOM_LEFT, 4, -22);

        m_ball_count = lv_label_create(lv_layer_top());

        lv_obj_set_style_text_color(m_ball_count, lv_color_white(), LV_STATE_DEFAULT);
        lv_obj_align(m_ball_count, LV_ALIGN_BOTTOM_LEFT, 4, -4);

        m_balls.reserve(initial_balls);

        reset_balls();

        for (size_t i = 0; i < 10; i++)
            m_voltage_level = 0.9f * m_voltage_level + 0.1f * hardware::battery::get().voltage_level();

        auto hud_update = [](lv_timer_t *timer)
        {
            static_cast<rc_link *>(timer->user_data)->update_hud();
        };

        lv_timer_create(hud_update, 200, this);
    }

    void on_update(float timestep) override
    {
        const auto min_x = 15;
        const auto min_y = 15;
        const auto max_x = m_width - 15;
        const auto max_y = m_height - 15;

        for (const auto ball : m_balls)
        {
            const auto flip_vx = (ball->position.x < min_x && ball->velocity.x < 0) || (ball->position.x > max_x && ball->velocity.x > 0);
            const auto flip_vy = (ball->position.y < min_y && ball->velocity.y < 0) || (ball->position.y > max_y && ball->velocity.y > 0);

            ball->velocity.x = flip_vx ? -ball->velocity.x : ball->velocity.x;
            ball->velocity.y = flip_vy ? -ball->velocity.y : ball->velocity.y;

            for (const auto b : m_balls)
            {
                if (b == ball)
                    continue;

                const auto dx = ball->position.x - b->position.x;
                const auto dy = ball->position.y - b->position.y;

                if ((dx * dx + dy * dy) < 900)
                    resolve_collision(*b, *ball);
            }

            ball->position.x += ball->velocity.x * timestep;
            ball->position.y += ball->velocity.y * timestep;

            lv_obj_set_pos(ball->obj_handle, ball->position.x - 15, ball->position.y - 15);
        }
    }

    void add_ball()
    {
        auto b = static_cast<ball *>(lv_mem_alloc(sizeof(ball)));

        b->obj_handle = lv_img_create(m_screen);

        b->position.x = (m_width / 2);
        b->position.y = (m_height / 2);
        b->velocity.x = lv_rand(50, 150);
        b->velocity.y = lv_rand(50, 150);

        if (lv_rand(0, 1))
            b->velocity.x = -b->velocity.x;

        if (lv_rand(0, 1))
            b->velocity.y = -b->velocity.y;

        lv_obj_set_size(b->obj_handle, 30, 30);
        lv_obj_set_pos(b->obj_handle, b->position.x - 15, b->position.y - 15);

        lv_obj_set_style_radius(b->obj_handle, 15, LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(b->obj_handle, 0, LV_STATE_DEFAULT);

        char path[] = "F:/images/ball_0.png";

        path[15] = '0' + lv_rand(0, 7);

        lv_img_set_src(b->obj_handle, path);

        m_balls.push_back(b);
    }

    void remove_ball()
    {
        if (!m_balls.size())
            return;

        auto b = m_balls.back();

        m_balls.pop_back();

        lv_obj_del(b->obj_handle);
        lv_mem_free(b);
    }

    void reset_balls()
    {
        if (m_timer)
            return;

        auto timer_cb = [](lv_timer_t *timer)
        {
            auto app = static_cast<rc_link *>(timer->user_data);

            if (app->m_balls.size() == initial_balls)
            {
                lv_timer_del(app->m_timer);

                app->m_timer = nullptr;

                return;
            }

            if (app->m_balls.size() < initial_balls)
                app->add_ball();
            else
                app->remove_ball();
        };

        m_timer = lv_timer_create(timer_cb, 100, this);
    }

    void update_hud()
    {
        auto &wifi = hardware::wifi::get();

        lv_label_set_text_fmt(m_ssid, "SSID: %s", wifi.get_ssid());
        lv_label_set_text_fmt(m_ip, "IP: %s", wifi.get_ip());
        lv_label_set_text_fmt(m_netmask, "Netmask: %s", wifi.get_netmask());
        lv_label_set_text_fmt(m_gatway, "Gateway: %s", wifi.get_gateway());

        m_voltage_level = 0.9f * m_voltage_level + 0.1f * hardware::battery::get().voltage_level();

        lv_label_set_text_fmt(m_battery_voltage, "Battery: %lumv", m_voltage_level);
        lv_label_set_text_fmt(m_ball_count, "Balls: %zu", m_balls.size());
    }

    void resolve_collision(ball &b1, ball &b2)
    {
        const float dx = b2.position.x - b1.position.x;
        const float dy = b2.position.y - b1.position.y;
        const float distance = std::sqrt(dx * dx + dy * dy);
        const float penetration_depth = (15 + 15) - distance;
        const float normal_x = dx / distance;
        const float normal_y = dy / distance;
        const float resolution_distance = penetration_depth / 2;

        b1.position.x -= normal_x * resolution_distance;
        b1.position.y -= normal_y * resolution_distance;
        b2.position.x += normal_x * resolution_distance;
        b2.position.y += normal_y * resolution_distance;

        const float relative_vx = b2.velocity.x - b1.velocity.x;
        const float relative_vy = b2.velocity.y - b1.velocity.y;
        const float v_along_normal = relative_vx * normal_x + relative_vy * normal_y;

        if (v_along_normal > 0)
            return;

        const float j = -(1 + 0.99f) * v_along_normal / 2;
        const float impulse_x = j * normal_x;
        const float impulse_y = j * normal_y;

        b1.velocity.x -= impulse_x;
        b1.velocity.y -= impulse_y;
        b2.velocity.x += impulse_x;
        b2.velocity.y += impulse_y;
    }

    sol::state m_sol_state;
    std::unique_ptr<http_server> mp_http_server;
    std::unique_ptr<websocket_server> mp_websocket_server;
    TaskHandle_t m_websocket_task;

    const uint16_t m_width;
    const uint16_t m_height;

    lv_group_t *m_group;
    lv_obj_t *m_screen;

    lv_obj_t *m_ssid;
    lv_obj_t *m_ip;
    lv_obj_t *m_netmask;
    lv_obj_t *m_gatway;
    lv_obj_t *m_battery_voltage;
    lv_obj_t *m_ball_count;

    lv_timer_t *m_timer = nullptr;

    std::vector<ball *> m_balls;
    uint32_t m_voltage_level = 0;
};

std::unique_ptr<application> create_application()
{
    return std::make_unique<rc_link>();
}