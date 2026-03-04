#include "core/camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

Camera::Camera() = default;

glm::vec3 Camera::forward() const {
    const float yawRad = glm::radians(m_yawDegrees);
    const float pitchRad = glm::radians(m_pitchDegrees);
    glm::vec3 dir{};
    dir.x = std::cos(yawRad) * std::cos(pitchRad);
    dir.y = std::sin(pitchRad);
    dir.z = std::sin(yawRad) * std::cos(pitchRad);
    return glm::normalize(dir);
}

glm::vec3 Camera::right() const {
    return glm::normalize(glm::cross(forward(), glm::vec3(0.0f, 1.0f, 0.0f)));
}

glm::mat4 Camera::viewMatrix() const {
    const glm::vec3 fwd = forward();
    return glm::lookAt(m_position, m_position + fwd, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 Camera::projectionMatrix(float aspectRatio) const {
    const float safeAspect = aspectRatio > 0.0f ? aspectRatio : 1.0f;
    glm::mat4 projection = glm::perspective(glm::radians(m_fovDegrees), safeAspect, m_nearPlane, m_farPlane);
    projection[1][1] *= -1.0f;  // Vulkan clip-space convention.
    return projection;
}

void Camera::processMouseMotion(float deltaX, float deltaY) {
    m_yawDegrees += deltaX * m_mouseSensitivity;
    m_pitchDegrees -= deltaY * m_mouseSensitivity;
    m_pitchDegrees = std::clamp(m_pitchDegrees, -89.0f, 89.0f);
}

void Camera::update(float deltaSeconds, const MovementInput& input) {
    if (deltaSeconds <= 0.0f) {
        return;
    }

    glm::vec3 movement{0.0f};
    const glm::vec3 fwd = forward();
    const glm::vec3 horizontalFwd = glm::normalize(glm::vec3(fwd.x, 0.0f, fwd.z));
    const glm::vec3 strafe = right();

    if (input.moveForward) {
        movement += horizontalFwd;
    }
    if (input.moveBackward) {
        movement -= horizontalFwd;
    }
    if (input.moveRight) {
        movement += strafe;
    }
    if (input.moveLeft) {
        movement -= strafe;
    }
    if (input.moveUp) {
        movement += glm::vec3(0.0f, 1.0f, 0.0f);
    }
    if (input.moveDown) {
        movement -= glm::vec3(0.0f, 1.0f, 0.0f);
    }

    if (glm::dot(movement, movement) <= 0.0f) {
        return;
    }

    const float speed = m_moveSpeed * (input.sprint ? m_sprintMultiplier : 1.0f);
    m_position += glm::normalize(movement) * speed * deltaSeconds;
}
