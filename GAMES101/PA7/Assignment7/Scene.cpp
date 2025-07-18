//
// Created by Göksu Güvendiren on 2019-05-14.
//

#include "Scene.hpp"


void Scene::buildBVH() {
    printf(" - Generating BVH...\n\n");
    this->bvh = new BVHAccel(objects, 1, BVHAccel::SplitMethod::NAIVE);
}

Intersection Scene::intersect(const Ray &ray) const
{
    return this->bvh->Intersect(ray);
}

void Scene::sampleLight(Intersection &pos, float &pdf) const
{
    float emit_area_sum = 0;
    for (uint32_t k = 0; k < objects.size(); ++k) {
        if (objects[k]->hasEmit()){
            emit_area_sum += objects[k]->getArea();
        }
    }
    float p = get_random_float() * emit_area_sum;
    emit_area_sum = 0;
    for (uint32_t k = 0; k < objects.size(); ++k) {
        if (objects[k]->hasEmit()){
            emit_area_sum += objects[k]->getArea();
            if (p <= emit_area_sum){
                objects[k]->Sample(pos, pdf);
                break;
            }
        }
    }
}

bool Scene::trace(
        const Ray &ray,
        const std::vector<Object*> &objects,
        float &tNear, uint32_t &index, Object **hitObject)
{
    *hitObject = nullptr;
    for (uint32_t k = 0; k < objects.size(); ++k) {
        float tNearK = kInfinity;
        uint32_t indexK;
        Vector2f uvK;
        if (objects[k]->intersect(ray, tNearK, indexK) && tNearK < tNear) {
            *hitObject = objects[k];
            tNear = tNearK;
            index = indexK;
        }
    }


    return (*hitObject != nullptr);
}

// Implementation of Path Tracing
// Vector3f Scene::castRay(const Ray &ray, int depth) const
// {
//     Intersection inter = intersect(ray);
//     if (!inter.happened) return Vector3f(0, 0, 0);

//     if (inter.m->hasEmission() && depth == 0) {
//         return inter.m->getEmission();
//     }

//     Vector3f L_dir(0, 0, 0), L_indir(0, 0, 0);

//     // 1. 直接光照
//     Intersection lightInter;
//     float pdf_light = 0.0f;
//     sampleLight(lightInter, pdf_light);
//     Vector3f x = lightInter.coords;
//     Vector3f ws = (x - inter.coords).normalized();
//     Vector3f NN = lightInter.normal;
//     Vector3f emit = lightInter.emit;

//     // shadow ray
//     Ray shadowRay(inter.coords, ws);
//     Intersection shadowInter = intersect(shadowRay);
//     if (shadowInter.happened && (shadowInter.coords - x).norm() < 1e-2) {
//         L_dir = emit * inter.m->eval(-ray.direction, ws, inter.normal)
//                 * dotProduct(ws, inter.normal)
//                 * dotProduct(-ws, NN)
//                 / dotProduct(x - inter.coords, x - inter.coords)
//                 / pdf_light;
//     }

//     // 2. 间接光照
//     if (get_random_float() < RussianRoulette) {
//         Vector3f wi = inter.m->sample(-ray.direction, inter.normal);
//         float pdf_hemi = inter.m->pdf(-ray.direction, wi, inter.normal);
//         if (pdf_hemi > 1e-6) {
//             Ray newRay(inter.coords, wi);
//             Intersection newInter = intersect(newRay);
//             if (newInter.happened && !newInter.m->hasEmission()) {
//                 L_indir = castRay(newRay, depth + 1)
//                           * inter.m->eval(-ray.direction, wi, inter.normal)
//                           * dotProduct(wi, inter.normal)
//                           / pdf_hemi
//                           / RussianRoulette;
//             }
//         }
//     }

//     return L_dir + L_indir;
// }
Vector3f Scene::castRay(const Ray &ray, int depth) const
{
    // TO DO Implement Path Tracing Algorithm here
    Intersection hit = intersect(ray);
    Vector3f L_dir, L_indir;
 
    if (!hit.happened)
        return L_dir;
    if (hit.m->hasEmission())return hit.m->getEmission();
 
    //接下来说明打到的是物体
    //将交点的信息取出来
    Vector3f p = hit.coords;
    Material* m = hit.m;
    Vector3f N = hit.normal;
    Vector3f w0 = ray.direction;   //注意这个入射方向是光射向着色点的！！！！！！
    w0.normalized();
 
    switch (m->getType())
    {
    case REFLC:
    {
        float prr = get_random_float();
        if (prr > RussianRoulette) return L_indir;
        
        Vector3f wi = m->sample(w0, N).normalized();
        Ray r(p + 0.0001, wi);
 
        Intersection hit2 = intersect(r);
        if (hit2.happened)
        {
            Vector3f brdf2 = m->eval(w0, wi, N);
            float cos_theta3 = dotProduct(wi, N);
            float pdf_ = m->pdf(w0, wi, N);
            if (pdf_ > 0.0001)
            {
                L_indir = castRay(r, depth + 1) * brdf2 * cos_theta3 / pdf_ / RussianRoulette;
            }
        }
        
        break;
    }
    case DIFFUSE:
    case MIRCO:
    {
 
 
        float pdf_L;
        Intersection light_hit;
        sampleLight(light_hit, pdf_L);//随机在所有光源上选出一个点，并且得到这个点的pdf
 
        //看看这个点可不可以照射到摄像头发出的光线打到的物体的点hit上
        Vector3f sa_l_coord = light_hit.coords;
        Vector3f sa_l_N = light_hit.normal;
        Vector3f dir_hit_to_sa_l = (sa_l_coord - p).normalized();
        Vector3f intencity_of_L = light_hit.emit;
        float d = (sa_l_coord - p).norm();
 
        Ray isBlock(p + 0.001, dir_hit_to_sa_l);
        Intersection hi = intersect(isBlock);
 
        if (hi.happened && hi.m->hasEmission()) {
            Vector3f brdf = m->eval(w0, dir_hit_to_sa_l, N);
            float cos_theta1 = dotProduct(N, dir_hit_to_sa_l);
            float cos_theta2 = dotProduct(sa_l_N, -dir_hit_to_sa_l);
            L_dir = (intencity_of_L * brdf * cos_theta1 * cos_theta2 / std::pow(d, 2)) / pdf_L;
        }
 
        //计算间接光照
        float prr = get_random_float();
        if (prr < RussianRoulette)
        {
            Vector3f wi = m->sample(w0, N).normalized();
            Ray ri(p, wi);
 
            Intersection hit2 = intersect(ri);
            if (hit2.happened && hit2.m->hasEmission() == false)
            {
                Vector3f brdf2 = m->eval(w0, wi, N);
                float cos_theta3 = dotProduct(wi, N);
                float pdf_ = m->pdf(w0, wi, N);
                L_indir = castRay(ri, depth + 1) * brdf2 * cos_theta3 / pdf_ / RussianRoulette;
            }
        }
    }
    }
 
    Vector3f res = L_dir + L_indir;
    res.x = clamp(0.0, 1.0, res.x);
    res.y = clamp(0.0, 1.0, res.y);
    res.z = clamp(0.0, 1.0, res.z);
 
    return res;
}