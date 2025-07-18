#include <iostream>
#include <vector>

#include "CGL/vector2D.h"

#include "mass.h"
#include "rope.h"
#include "spring.h"

namespace CGL {

    Rope::Rope(Vector2D start, Vector2D end, int num_nodes, float node_mass, float k, vector<int> pinned_nodes)
    {
        // TODO (Part 1): Create a rope starting at `start`, ending at `end`, and containing `num_nodes` nodes.
        Vector2D distance = (end - start)/(num_nodes - 1);
        for (int i = 0; i < num_nodes; ++i)
        {
            Mass *mass = new Mass(start + distance * i, node_mass, false);
            masses.push_back(mass);
            if (i > 0)
            {
                Mass *prev_mass = masses[i - 1];
                Spring *spring = new Spring(prev_mass, mass, k);
                springs.push_back(spring);
            }
        }
       //Comment-in this part when you implement the constructor
        for (auto &i : pinned_nodes) {
           masses[i]->pinned = true;
        }

    }

    void Rope::simulateEuler(float delta_t, Vector2D gravity)
    {
        for (auto &s : springs)
        {
            // TODO (Part 2): Use Hooke's law to calculate the force on a node
            Vector2D direction = s->m2->position - s->m1->position;
            double length = direction.norm();
            Vector2D force = direction.unit() * (length - s->rest_length) * s->k;
            s->m1->forces += force;
            s->m2->forces -= force; // Equal and opposite force on the second mass
        }

        for (auto &m : masses)
        {
            float k_d = 0.1f; // Damping coefficient
            if (!m->pinned)
            {
                // TODO (Part 2): Add the force due to gravity, then compute the new velocity and position
                m->forces += gravity * m->mass; // Gravity
                m->forces += - k_d * m->velocity; // Damping force
                Vector2D acceleration = m->forces / m->mass;
                /*
                    //explicit Euler integration
                m->position += m->velocity * delta_t; // Update position
                m->velocity += acceleration * delta_t; // Update velocity
                */
               
                    // implicit Euler integration
                m->velocity += acceleration * delta_t; // Update velocity
                m->position += m->velocity * delta_t; // Update position

                // TODO (Part 2): Add global damping
            }

            // Reset all forces on each mass
            m->forces = Vector2D(0, 0);
        }
    }

    void Rope::simulateVerlet(float delta_t, Vector2D gravity)
    {
        for (auto &s : springs)
        {
            // TODO (Part 3): Simulate one timestep of the rope using explicit Verlet （solving constraints)
            Vector2D direction = s->m2->position - s->m1->position;
            double length = direction.norm();
            Vector2D force = direction.unit() * (length - s->rest_length) * s->k;
            s->m1->forces += force;
            s->m2->forces -= force; // Equal and opposite force on the second mass

        }

        for (auto &m : masses)
        {
            if (!m->pinned)
            {
                m->forces += gravity * m->mass; // Gravity
                Vector2D acceleration = m->forces / m->mass;
                
                // TODO (Part 3.1): Set the new position of the rope mass
                Vector2D temp_position = m->position;
                // TODO (Part 4): Add global Verlet damping
                double  damping_factor = 0.00005;
                m->position += (1- damping_factor) * (m->position - m->last_position) + acceleration * delta_t * delta_t;
                m->last_position = temp_position; // Update last position
            }
            // Reset all forces on each mass
            m->forces = Vector2D(0, 0);
        }
    }
}
