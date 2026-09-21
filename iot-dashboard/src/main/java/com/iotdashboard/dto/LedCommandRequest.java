package com.iotdashboard.dto;

import jakarta.validation.constraints.NotBlank;
import jakarta.validation.constraints.Pattern;

public record LedCommandRequest(
        @NotBlank(message = "State is required")
        @Pattern(regexp = "on|off", message = "State must be 'on' or 'off'")
        String state
) {}
