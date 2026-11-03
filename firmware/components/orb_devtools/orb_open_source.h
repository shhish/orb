#pragma once

#include "orb_shell.h"

/* Source navigation is outside the measured interval. Start from Face and
 * settle each step so a previous sample's destination/history cannot turn the
 * next injection into a same-page refresh or send Back to the wrong surface. */
typedef struct {
    orb_shell_surface_t source, stage;
} orb_open_source_t;

static inline bool orb_open_source_settled(orb_shell_t *shell, orb_shell_surface_t surface)
{
    orb_shell_open_metrics_t metrics;
    orb_shell_open_metrics(shell, &metrics);
    return orb_shell_surface(shell) == surface && metrics.surface == surface &&
           !metrics.active && metrics.content_ready;
}

static inline void orb_open_source_begin(orb_open_source_t *setup, orb_shell_t *shell,
                                         orb_shell_surface_t source)
{
    setup->source = source;
    setup->stage = ORB_SHELL_FACE;
    orb_shell_navigate(shell, setup->stage);
}

static inline bool orb_open_source_ready(orb_open_source_t *setup, orb_shell_t *shell)
{
    if (!orb_open_source_settled(shell, setup->stage)) return false;
    if (setup->stage == setup->source) return true;
    setup->stage = setup->stage == ORB_SHELL_FACE ? ORB_SHELL_APPS : ORB_SHELL_PLAY;
    orb_shell_navigate(shell, setup->stage);
    return false;
}
