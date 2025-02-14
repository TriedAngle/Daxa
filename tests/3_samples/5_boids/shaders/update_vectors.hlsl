#include "daxa/daxa.hlsl"

#include "boids.hlsl"

struct Push
{
    daxa::BufferId prev_boids_buffer_id;
    daxa::BufferId boids_buffer_id;
    float delta_time;
};
[[vk::push_constant]] Push push;

[numthreads(64, 1, 1)] void main(uint tid
                                 : SV_DISPATCHTHREADID)
{
    if (tid >= 100 * 100)
        return;

    StructuredBuffer<BoidsBuffer> prev_boids_buffer = daxa::get_StructuredBuffer<BoidsBuffer>(push.prev_boids_buffer_id);
    StructuredBuffer<BoidsBuffer> boids_buffer = daxa::get_StructuredBuffer<BoidsBuffer>(push.boids_buffer_id);

    BoidState me = prev_boids_buffer[0].boid_states[tid];

    float2 vel = float2(0, 0);

    vel += prev_boids_buffer[0].eval_field(tid, me.pos, vel);

    me.speed = length(vel);
    me.dir = vel / me.speed;
    me.speed = clamp(me.speed, 0.01, 0.1);

    vel = me.dir * me.speed;

    // me.pos += vel * push.delta_time;
    // me.pos = frac(me.pos * 0.5 + 0.5) * 2.0 - 1.0;

    uint xi = tid % 100;
    uint yi = tid / 100;

    me.pos = float2(xi, yi) * 0.01 * 2.0 - 1.0;

    boids_buffer[0].boid_states[tid].pos = me.pos;
    // boids_buffer[0].boid_states[tid].dir = me.dir;
    // boids_buffer[0].boid_states[tid].speed = me.speed;
}
