#pragma once

#include <glm/glm.hpp>

class Camera {
public:
    struct MovementInput {
        bool moveForward = false;
        bool moveBackward = false;
        bool moveLeft = false;
        bool moveRight = false;
        bool moveUp = false;
        bool moveDown = false;
        bool sprint = false;
    };

    Camera();

    glm::mat4 viewMatrix() const;
    glm::mat4 projectionMatrix(float aspectRatio) const;

    void processMouseMotion(float deltaX, float deltaY);
    void update(float deltaSeconds, const MovementInput& input);

    const glm::vec3& position() const { return m_position; }

private:
    glm::vec3 forward() const;
    glm::vec3 right() const;

    glm::vec3 m_position{4.0f, 4.0f, 6.0f};
    float m_yawDegrees = -141.340194f;
    float m_pitchDegrees = -22.038736f;
    float m_fovDegrees = 50.0f;
    float m_nearPlane = 0.1f;
    float m_farPlane = 512.0f;
    float m_moveSpeed = 3.5f;
    float m_sprintMultiplier = 2.25f;
    float m_mouseSensitivity = 0.10f;
};
