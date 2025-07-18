//
// Created by LEI XU on 5/16/19.
//

#ifndef RAYTRACING_MATERIAL_H
#define RAYTRACING_MATERIAL_H

#include "Vector.hpp"

enum MaterialType { DIFFUSE,MIRCO, REFLC};

class Material{
private:

    // Compute reflection direction
    Vector3f reflect(const Vector3f &I, const Vector3f &N) const
    {
        return I - 2 * dotProduct(I, N) * N;
    }

    // Compute refraction direction using Snell's law
    //
    // We need to handle with care the two possible situations:
    //
    //    - When the ray is inside the object
    //
    //    - When the ray is outside.
    //
    // If the ray is outside, you need to make cosi positive cosi = -N.I
    //
    // If the ray is inside, you need to invert the refractive indices and negate the normal N
    Vector3f refract(const Vector3f &I, const Vector3f &N, const float &ior) const
    {
        float cosi = clamp(-1, 1, dotProduct(I, N));
        float etai = 1, etat = ior;
        Vector3f n = N;
        if (cosi < 0) { cosi = -cosi; } else { std::swap(etai, etat); n= -N; }
        float eta = etai / etat;
        float k = 1 - eta * eta * (1 - cosi * cosi);
        return k < 0 ? 0 : eta * I + (eta * cosi - sqrtf(k)) * n;
    }

    // Compute Fresnel equation
    //
    // \param I is the incident view direction
    //
    // \param N is the normal at the intersection point
    //
    // \param ior is the material refractive index
    //
    // \param[out] kr is the amount of light reflected
    void fresnel(const Vector3f &I, const Vector3f &N, const float &ior, float &kr) const
    {
        float cosi = clamp(-1, 1, dotProduct(I, N));
        float etai = 1, etat = ior;
        if (cosi > 0) {  std::swap(etai, etat); }
        // Compute sini using Snell's law
        float sint = etai / etat * sqrtf(std::max(0.f, 1 - cosi * cosi));
        // Total internal reflection
        if (sint >= 1) {
            kr = 1;
        }
        else {
            float cost = sqrtf(std::max(0.f, 1 - sint * sint));
            cosi = fabsf(cosi);
            float Rs = ((etat * cosi) - (etai * cost)) / ((etat * cosi) + (etai * cost));
            float Rp = ((etai * cosi) - (etat * cost)) / ((etai * cosi) + (etat * cost));
            kr = (Rs * Rs + Rp * Rp) / 2;
        }
        // As a consequence of the conservation of energy, transmittance is given by:
        // kt = 1 - kr;
    }

    Vector3f toWorld(const Vector3f &a, const Vector3f &N){
        Vector3f B, C;
        if (std::fabs(N.x) > std::fabs(N.y)){
            float invLen = 1.0f / std::sqrt(N.x * N.x + N.z * N.z);
            C = Vector3f(N.z * invLen, 0.0f, -N.x *invLen);
        }
        else {
            float invLen = 1.0f / std::sqrt(N.y * N.y + N.z * N.z);
            C = Vector3f(0.0f, N.z * invLen, -N.y *invLen);
        }
        B = crossProduct(C, N);
        return a.x * B + a.y * C + a.z * N;
    }

public:
    MaterialType m_type;
    //Vector3f m_color;
    Vector3f m_emission;
    float ior;
    Vector3f Kd, Ks;
    float specularExponent;
    //Texture tex;
    float roughness;
    inline Material(MaterialType t=DIFFUSE, Vector3f e=Vector3f(0,0,0));
    inline MaterialType getType();
    //inline Vector3f getColor();
    inline Vector3f getColorAt(double u, double v);
    inline Vector3f getEmission();
    inline bool hasEmission();

    // sample a ray by Material properties
    inline Vector3f sample(const Vector3f &wi, const Vector3f &N);
    // given a ray, calculate the PdF of this ray
    inline float pdf(const Vector3f &wi, const Vector3f &wo, const Vector3f &N);
    // given a ray, calculate the contribution of this ray
    inline Vector3f eval(const Vector3f &wi, const Vector3f &wo, const Vector3f &N);

    inline float G_s(float NdotV, float a);
    inline float G(Vector3f i, Vector3f o, Vector3f N , float a);
    inline float D_GGX(Vector3f h, float r, Vector3f N);


};

float Material::G_s(float NdotV, float a)
{
    float k = (a + 1) * (a + 1) / 8;
    return NdotV / (NdotV* (1- k) + k );
}
 
float Material::G(Vector3f i, Vector3f o, Vector3f N , float a)
{
    float NdotI = clamp(0.0, 1.0, dotProduct(N, i));
    float NdotO = clamp(0.0, 1.0, dotProduct(N, o));
    return G_s(NdotI, a) * G_s(NdotO , a);
}

float Material::D_GGX(Vector3f h, float r, Vector3f N)  
{
    float r2 = r * r;
    float NdotH = clamp(0.0, 1.0, dotProduct(N, h));  //有点乘就需要约束！！
    float NdotH2 = NdotH * NdotH;
    float res = r2 / (M_PI * (NdotH2 * (r2 - 1) + 1) * (NdotH2 * (r2 - 1) + 1));
    return res;
}

Material::Material(MaterialType t, Vector3f e){
    m_type = t;
    //m_color = c;
    m_emission = e;
}

MaterialType Material::getType(){return m_type;}
///Vector3f Material::getColor(){return m_color;}
Vector3f Material::getEmission() {return m_emission;}
bool Material::hasEmission() {
    if (m_emission.norm() > EPSILON) return true;
    else return false;
}

Vector3f Material::getColorAt(double u, double v) {
    return Vector3f();
}


Vector3f Material::sample(const Vector3f &wi, const Vector3f &N){
    switch(m_type){
        case MIRCO:
        case DIFFUSE:
        {
            // uniform sample on the hemisphere
            float x_1 = get_random_float(), x_2 = get_random_float();
            float z = std::fabs(1.0f - 2.0f * x_1);
            float r = std::sqrt(1.0f - z * z), phi = 2 * M_PI * x_2;
            Vector3f localRay(r*std::cos(phi), r*std::sin(phi), z);
            return toWorld(localRay, N);
            
            break;
        }
        case REFLC:
        {
            Vector3f localRay = reflect(wi, N).normalized();
            return localRay;
            break;
        }
    }
}

float Material::pdf(const Vector3f &wi, const Vector3f &wo, const Vector3f &N){
    switch(m_type){
        case MIRCO:
        case DIFFUSE:
        {
            // uniform sample probability 1 / (2 * PI)
            if (dotProduct(wo, N) > 0.0f)
                return 0.5f / M_PI;
            else
                return 0.0f;
            break;
        }
        case REFLC:
        {
            if(dotProduct(wo,N)>0.0001f)
            {
                return 1.0f; // 反射光线的概率密度函数
            }
            else return 0.0f; // 反射光线的概率密度函数为0
            break;
        }
    }
}

Vector3f Material::eval(const Vector3f &wi, const Vector3f &wo, const Vector3f &N){
    switch(m_type){
        case DIFFUSE:
        {
            // calculate the contribution of diffuse   model
            float cosalpha = dotProduct(N, wo);
            if (cosalpha > 0.0f) {
                Vector3f diffuse = Kd / M_PI;
                return diffuse;
            }
            else
                return Vector3f(0.0f);
            break;
        }
        case MIRCO:
        {
            float cosalpha = dotProduct(N, wo); //wo是观测方向
            if (cosalpha > 0.0f) {
                Vector3f h = (wi + wo).normalized();
                float F = 0.f;
                fresnel(wi, N, ior, F);
                float NdotI = clamp(0.0, 1.0, dotProduct(N, wi));
                float NdotO = clamp(0.0, 1.0, dotProduct(N, wo));
                float down = 4 * NdotI * NdotO + 1e-5;
                float up = F * G(wi, wo, N, roughness) * D_GGX(h, roughness, N);
                return (up / down) * Kd; //加上Kd就实现了颜色，当然主函数记得给材质设置Kd
            }
            else
                return Vector3f(0.0f);
            break;
        }
        case REFLC:
        {
            float cosa = dotProduct(N, wo);
            if (cosa > 0.0001f)
            {
                float K = 0.0;
                fresnel(wi, N ,ior , K);
                Vector3f t = (1.0 / cosa) ;
                t = t * Kd;
                return t * K;
            }
            break;
        }
    }
}

#endif //RAYTRACING_MATERIAL_H
