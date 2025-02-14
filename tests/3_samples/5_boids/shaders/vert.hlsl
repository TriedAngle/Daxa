#include "daxa/daxa.hlsl"

#include "boids.hlsl"
#include "common.hlsl"

struct Push
{
    float4x4 view_mat;
    daxa::BufferId boids_buffer_id;
};
[[vk::push_constant]] const Push p;

VertexOutput main(uint vert_i : SV_VERTEXID)
{
    StructuredBuffer<BoidsBuffer> boids_buffer = daxa::get_StructuredBuffer<BoidsBuffer>(p.boids_buffer_id);
    BoidVertex vert = boids_buffer[0].get_vertex(vert_i);

    VertexOutput result;
    result.frag_pos = mul(p.view_mat, float4(vert.pos.xy, 0, 1));
    result.col = float4(vert.col, 1);
    return result;
}
