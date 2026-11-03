#include "orb_screen_private.h"

#include <math.h>
#include <string.h>

static bool active(const orb_screen_data_t *data)
{
    return data && !data->destroying && !data->preview && !data->silent &&
        data->visibility == ORB_VISIBILITY_ACTIVE;
}

static orb_nav_t *rim_nav(const orb_screen_host_t *host)
{
    return host->config.rim_nav ? host->config.rim_nav : host->config.nav;
}

/* The shared navigation still carries this host's binding of the same
 * shape: its owner may have rebound it, or a recreated screen may follow. */
static bool bound_here(const orb_screen_host_t *host, const orb_screen_data_t *data,
    const orb_rim_binding_t *binding)
{
    const orb_nav_t *nav = rim_nav(host);
    const orb_nav_rotary_owner_t *owner = &nav->rotary.owner;
    return nav->rotary.bound && owner->ctx == host && owner->owner == data->descriptor->id &&
        owner->minimum == binding->minimum && owner->maximum == binding->maximum &&
        owner->detent == binding->detent && owner->units_per_turn == binding->units_per_turn;
}

static bool valid_binding(const orb_rim_binding_t *binding)
{
    return binding->property && binding->kind >= ORB_RIM_SCROLL && binding->kind <= ORB_RIM_GAME_LOCAL &&
        binding->begin && binding->preview && binding->finish &&
        isfinite(binding->minimum) && isfinite(binding->maximum) && isfinite(binding->value) &&
        isfinite(binding->detent) && isfinite(binding->units_per_turn) &&
        binding->maximum >= binding->minimum && binding->value >= binding->minimum &&
        binding->value <= binding->maximum && binding->detent > 0 && binding->units_per_turn > 0;
}

void orb_screen_rim_callback(orb_screen_data_t *data, bool begin, bool finish, bool commit)
{
    orb_rim_lease_t *lease = &data->host->lease;
    data->rim_callback = true;
    data->origin = lease->origin;
    data->physical = lease->physical;
    if (!finish) data->silent++;
    if (begin) lease->binding.begin(data->handle, lease->origin, lease->physical);
    else if (finish) lease->binding.finish(data->handle, lease->binding.value, commit,
                                         lease->origin, lease->physical);
    else lease->binding.preview(data->handle, lease->binding.value);
    if (!finish) data->silent--;
    data->rim_callback = false;
}

static bool shared_contact(orb_screen_host_t *host)
{
    bool physical = false;
    return host->config.rim_nav && host->config.rim_contact &&
        host->config.rim_contact(host->config.context, &physical) &&
        orb_screen_rim_begin(host->lease.instance, ORB_INPUT_CROWN, physical);
}

static void rotary_change(void *context, float value, float delta)
{
    orb_screen_host_t *host = context;
    orb_screen_data_t *data = orb_screen_data(host->lease.instance);
    if (!active(data) || (!host->lease.contact && !shared_contact(host)) ||
        host->lease.origin != ORB_INPUT_CROWN) return;
    host->lease.binding.value = value;
    orb_screen_rim_callback(data, false, false, false);
}

static void rotary_tick(void *context)
{
    orb_screen_host_t *host = context;
    orb_screen_instance_t *instance = host->lease.instance;
    /* A shared shell recogniser has always ticked every detent of a captured
     * contact, including a developer pointer's; keep that receipt there. */
    if (!instance || !host->lease.contact || (!host->lease.physical && !host->config.rim_nav)) return;
    orb_screen_feedback(instance, orb_screen_event(host), ORB_CUE_DETENT, ORB_INPUT_CROWN, true);
}

bool orb_screen_rim_acquire(orb_screen_instance_t *instance)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    if (!active(data) || !rim_nav(data->host)) return false;
    orb_screen_host_t *host = data->host;
    if (host->lease.contact) return false;
    orb_rim_binding_t binding = {0};
    data->silent++;
    bool bound = data->descriptor->ops->rim_binding(instance, &binding);
    data->silent--;
    if (!bound || !valid_binding(&binding)) {
        if (host->lease.instance == instance) orb_screen_rim_release(host);
        return false;
    }
    if (host->lease.instance && host->lease.instance != instance) return false;
    orb_nav_t *nav = rim_nav(host);
    if (host->config.rim_nav) {
        if (bound_here(host, data, &binding)) {
            /* Keep the angle, fractional detents and fling of a captured or
             * coasting turn; otherwise follow the property's current value. */
            host->lease = (orb_rim_lease_t){.instance = instance, .binding = binding};
            if (!nav->touch.down && !nav->rotary.flinging) orb_nav_rotary_set(nav, binding.value);
            return true;
        }
        /* Rebinding replaces the shared owner in place. */
        memset(&host->lease, 0, sizeof(host->lease));
    } else orb_screen_rim_release(host);
    host->lease = (orb_rim_lease_t){.instance = instance, .binding = binding};
    orb_nav_rotary_owner_t owner = {
        .owner = data->descriptor->id, .ctx = host, .change = rotary_change, .tick = rotary_tick,
        .minimum = binding.minimum, .maximum = binding.maximum, .value = binding.value,
        .detent = binding.detent, .units_per_turn = binding.units_per_turn,
    };
    orb_nav_rotary_bind(nav, &owner);
    return true;
}

void orb_screen_rim_release(orb_screen_host_t *host)
{
    if (!host || !host->lease.instance) return;
    orb_screen_instance_t *instance = host->lease.instance;
    orb_screen_data_t *data = orb_screen_data(instance);
    if (host->lease.contact && data) {
        host->lease.binding.value = host->lease.start;
        data->silent++;
        orb_screen_rim_callback(data, false, true, false);
        data->silent--;
    }
    if (!host->config.rim_nav && host->config.nav && data) orb_nav_rotary_unbind(host->config.nav, data->descriptor->id);
    memset(&host->lease, 0, sizeof(host->lease));
}

bool orb_screen_rim_begin(orb_screen_instance_t *instance, orb_input_origin_t origin, bool physical)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    if (!active(data) || (origin != ORB_INPUT_TOUCH && origin != ORB_INPUT_CROWN)) return false;
    orb_rim_lease_t *lease = &data->host->lease;
    if (lease->instance != instance || lease->contact || (lease->binding.physical_required && !physical)) return false;
    lease->contact = true;
    lease->origin = origin;
    lease->physical = physical;
    lease->start = lease->binding.value;
    if (rim_nav(data->host)) orb_nav_rotary_stop(rim_nav(data->host));
    orb_screen_rim_callback(data, true, false, false);
    return true;
}

bool orb_screen_rim_set(orb_screen_instance_t *instance, float value, orb_input_origin_t origin)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    if (!active(data) || !isfinite(value)) return false;
    orb_rim_lease_t *lease = &data->host->lease;
    if (lease->instance != instance || (lease->contact && origin != lease->origin) ||
        (!lease->contact && origin != ORB_INPUT_PROGRAMMATIC)) return false;
    value = fminf(lease->binding.maximum, fmaxf(lease->binding.minimum, value));
    if (lease->binding.value == value) return true;
    float previous = lease->binding.value;
    lease->binding.value = value;
    if (rim_nav(data->host)) orb_nav_rotary_set(rim_nav(data->host), value);
    orb_screen_rim_callback(data, false, false, false);
    if (origin != ORB_INPUT_PROGRAMMATIC && lease->physical &&
        floorf((previous - lease->binding.minimum) / lease->binding.detent) !=
        floorf((value - lease->binding.minimum) / lease->binding.detent))
        orb_screen_feedback(instance, orb_screen_event(data->host), ORB_CUE_DETENT, origin, true);
    return true;
}

bool orb_screen_rim_finish(orb_screen_instance_t *instance, bool commit)
{
    orb_screen_data_t *data = orb_screen_data(instance);
    if (!active(data)) return false;
    orb_rim_lease_t *lease = &data->host->lease;
    if (lease->instance != instance || !lease->contact) return false;
    if (!commit) lease->binding.value = lease->start;
    if (!commit) data->silent++;
    orb_screen_rim_callback(data, false, true, commit);
    if (!commit) data->silent--;
    lease->contact = false;
    if (rim_nav(data->host)) orb_nav_rotary_set(rim_nav(data->host), lease->binding.value);
    return true;
}

bool orb_screen_rim_step(orb_screen_host_t *host, int detents, bool physical)
{
    if (!host || !rim_nav(host) || !host->lease.instance || !detents) return false;
    orb_screen_instance_t *instance = host->lease.instance;
    if (!orb_screen_rim_begin(instance, ORB_INPUT_CROWN, physical)) return false;
    bool changed = orb_nav_rotary_step(rim_nav(host), detents);
    orb_screen_rim_finish(instance, changed);
    return changed;
}
