/*
 * * Copyright (C) 2009-2011 Ali <aliov@xfce.org>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#include <upower.h>

#include <libxfce4util/libxfce4util.h>

#include "xfpm-battery.h"
#include "xfpm-dbus.h"
#include "xfpm-icons.h"
#include "xfpm-xfconf.h"
#include "xfpm-notify.h"
#include "xfpm-config.h"
#include "xfpm-button.h"
#include "xfpm-enum-glib.h"
#include "xfpm-enum-types.h"
#include "xfpm-debug.h"
#include "xfpm-power-common.h"
#include "xfpm-common.h"

static void xfpm_battery_finalize   (GObject *object);

#define XFPM_BATTERY_GET_PRIVATE(o) \
(G_TYPE_INSTANCE_GET_PRIVATE ((o), XFPM_TYPE_BATTERY, XfpmBatteryPrivate))

struct XfpmBatteryPrivate
{
    XfpmXfconf             *conf;
    XfpmNotify		   *notify;
    XfpmButton             *button;
    UpDevice               *device;
    UpClient               *client;

    gchar		   *icon_prefix;

    XfpmBatteryCharge       charge;
    UpDeviceState	    state;
    UpDeviceKind	    type;
    gboolean		    ac_online;
    gboolean                present;
    guint 		    percentage;
    gint64		    time_to_full;
    gint64		    time_to_empty;

    const gchar            *battery_name;

    gulong		    sig;
    gulong		    sig_bt;
    gulong		    sig_up;

    guint                   notify_idle;
};

enum
{
    PROP_0,
    PROP_AC_ONLINE,
    PROP_CHARGE_STATUS,
    PROP_DEVICE_TYPE
};

enum
{
    BATTERY_CHARGE_CHANGED,
    LAST_SIGNAL
};

static guint signals [LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (XfpmBattery, xfpm_battery, GTK_TYPE_STATUS_ICON)

static void
xfpm_battery_refresh_visible (XfpmBattery *battery)
{
    XfpmShowIcon show_icon;
    gboolean visible = TRUE;

    g_object_get (G_OBJECT (battery->priv->conf),
		  SHOW_TRAY_ICON_CFG, &show_icon,
		  NULL);

    if ( show_icon == SHOW_ICON_ALWAYS )
    	visible = TRUE;
    else if ( show_icon == NEVER_SHOW_ICON )
	visible = FALSE;
    else if ( show_icon == SHOW_ICON_WHEN_BATTERY_PRESENT )
    {
	if ( battery->priv->present )
	    visible = TRUE;
	else visible = FALSE;
    }
    else if ( show_icon == SHOW_ICON_WHEN_BATTERY_CHARGING_DISCHARGING )
    {
	if ( battery->priv->state == UP_DEVICE_STATE_FULLY_CHARGED )
	    visible = FALSE;
	else visible = TRUE;
    }

    XFPM_DEBUG_ENUM (show_icon, XFPM_TYPE_SHOW_ICON, "visible=%s", xfpm_bool_to_string (visible));

    gtk_status_icon_set_visible (GTK_STATUS_ICON (battery), visible);
}

static gchar *
xfpm_battery_get_message_from_battery_state (XfpmBattery *battery)
{
    gchar *msg  = NULL;


    if (battery->priv->type == UP_DEVICE_KIND_BATTERY || battery->priv->type == UP_DEVICE_KIND_UPS)
    {
	switch (battery->priv->state)
	{
	    case UP_DEVICE_STATE_FULLY_CHARGED:
		msg = g_strdup_printf (_("Your %s is fully charged"), battery->priv->battery_name);
		break;
	    case UP_DEVICE_STATE_CHARGING:
		msg = g_strdup_printf (_("Your %s is charging"), battery->priv->battery_name);

		if ( battery->priv->time_to_full != 0 )
		{
		    gchar *tmp, *est_time_str;
		    tmp = g_strdup (msg);
		    g_free (msg);

		    est_time_str = xfpm_battery_get_time_string (battery->priv->time_to_full);

		    msg = g_strdup_printf (_("%s (%i%%)\n%s until it is fully charged."), tmp, battery->priv->percentage, est_time_str);
		    g_free (est_time_str);
		    g_free (tmp);
		}

		break;
	    case UP_DEVICE_STATE_DISCHARGING:
		if (battery->priv->ac_online)
		    msg =  g_strdup_printf (_("Your %s is discharging"), battery->priv->battery_name);
		else
		    msg =  g_strdup_printf (_("System is running on %s power"), battery->priv->battery_name);

		    if ( battery->priv->time_to_empty != 0 )
		    {
			gchar *tmp, *est_time_str;
			tmp = g_strdup (msg);
			g_free (msg);

			est_time_str = xfpm_battery_get_time_string (battery->priv->time_to_empty);

			msg = g_strdup_printf (_("%s (%i%%)\nEstimated time left is %s."), tmp, battery->priv->percentage, est_time_str);
			g_free (tmp);
			g_free (est_time_str);
		    }
		break;
	    case UP_DEVICE_STATE_EMPTY:
		msg = g_strdup_printf (_("Your %s is empty"), battery->priv->battery_name);
		break;
	    default:
		break;
	}

    }
    else if (battery->priv->type >= UP_DEVICE_KIND_MONITOR)
    {
	switch (battery->priv->state)
	{
	    case UP_DEVICE_STATE_FULLY_CHARGED:
		msg = g_strdup_printf (_("Your %s is fully charged"), battery->priv->battery_name);
		break;
	    case UP_DEVICE_STATE_CHARGING:
		msg = g_strdup_printf (_("Your %s is charging"), battery->priv->battery_name);
		break;
	    case UP_DEVICE_STATE_DISCHARGING:
		msg =  g_strdup_printf (_("Your %s is discharging"), battery->priv->battery_name);
		break;
	    case UP_DEVICE_STATE_EMPTY:
		msg = g_strdup_printf (_("Your %s is empty"), battery->priv->battery_name);
		break;
	    default:
		break;
	}
    }

    return msg;
}

static void
xfpm_battery_refresh_icon (XfpmBattery *battery)
{
    gchar icon_name[128];

    XFPM_DEBUG ("Battery state %d", battery->priv->state);

    if ( battery->priv->type == UP_DEVICE_KIND_BATTERY ||
	 battery->priv->type == UP_DEVICE_KIND_UPS )
    {
	if (!battery->priv->present)
	{
	    g_snprintf (icon_name, 128, "%s%s", battery->priv->icon_prefix, "missing");
	}
	else if (battery->priv->state == UP_DEVICE_STATE_FULLY_CHARGED )
	{
	    g_snprintf (icon_name, 128, "%s%s", battery->priv->icon_prefix, battery->priv->ac_online ? "charged" : "100");
	}
	else if ( battery->priv->state == UP_DEVICE_STATE_CHARGING ||
		  battery->priv->state == UP_DEVICE_STATE_PENDING_CHARGE)
	{
	    g_snprintf (icon_name, 128, "%s%s-%s",
			battery->priv->icon_prefix,
			xfpm_battery_get_icon_index (battery->priv->type, battery->priv->percentage),
			"charging");
	}
	else if ( battery->priv->state == UP_DEVICE_STATE_DISCHARGING ||
		  battery->priv->state == UP_DEVICE_STATE_PENDING_DISCHARGE)
	{
	    g_snprintf (icon_name, 128, "%s%s",
			battery->priv->icon_prefix,
			xfpm_battery_get_icon_index (battery->priv->type, battery->priv->percentage));
	}
	else if ( battery->priv->state == UP_DEVICE_STATE_EMPTY)
	{
	    g_snprintf (icon_name, 128, "%s%s", battery->priv->icon_prefix, battery->priv->ac_online ? "000-charging" : "000");
	}
    }
    else
    {
	if ( !battery->priv->present || battery->priv->state == UP_DEVICE_STATE_EMPTY )
	{
	    g_snprintf (icon_name, 128, "%s000", battery->priv->icon_prefix);
	}
	else if ( battery->priv->state == UP_DEVICE_STATE_FULLY_CHARGED )
	{
	    g_snprintf (icon_name, 128, "%s100", battery->priv->icon_prefix);
	}
	else if ( battery->priv->state == UP_DEVICE_STATE_DISCHARGING || battery->priv->state == UP_DEVICE_STATE_CHARGING )
	{
	    g_snprintf (icon_name, 128, "%s%s",
			battery->priv->icon_prefix,
			xfpm_battery_get_icon_index (battery->priv->type, battery->priv->percentage));
	}
    }

    XFPM_DEBUG ("Battery icon %s", icon_name);

    gtk_status_icon_set_from_icon_name (GTK_STATUS_ICON (battery), icon_name);
}

static void
xfpm_battery_notify (XfpmBattery *battery)
{
    gchar *message = NULL;

    message = xfpm_battery_get_message_from_battery_state (battery);

    if ( !message )
	return;

    xfpm_notify_show_notification (battery->priv->notify,
				   _("Power Manager"),
				   message,
				   gtk_status_icon_get_icon_name (GTK_STATUS_ICON (battery)),
				   8000,
				   FALSE,
				   XFPM_NOTIFY_NORMAL,
				   GTK_STATUS_ICON (battery));

    g_free (message);
}

static gboolean
xfpm_battery_notify_idle (gpointer data)
{
    XfpmBattery *battery = XFPM_BATTERY (data);

    xfpm_battery_notify (battery);
    battery->priv->notify_idle = 0;

    return FALSE;
}

static void
xfpm_battery_notify_state (XfpmBattery *battery)
{
    gboolean notify;
    static gboolean starting_up = TRUE;

    if ( !gtk_status_icon_get_visible (GTK_STATUS_ICON (battery)) )
	return;

    if ( battery->priv->type == UP_DEVICE_KIND_BATTERY ||
	 battery->priv->type == UP_DEVICE_KIND_UPS )
    {
	if ( starting_up )
	{
	    starting_up = FALSE;
	    return;
	}

	g_object_get (G_OBJECT (battery->priv->conf),
		      GENERAL_NOTIFICATION_CFG, &notify,
		      NULL);

	if ( notify )
	{
	    if (battery->priv->notify_idle == 0)
	        battery->priv->notify_idle = g_idle_add (xfpm_battery_notify_idle, battery);
	}
    }
}

/*
 * Refresh tooltip function for devices with a battery.
 */
static void
xfpm_battery_set_tooltip_primary (XfpmBattery *battery, GtkTooltip *tooltip)
{
    gchar *tip = NULL;
    gchar *est_time_str = NULL;
    gchar *power_status = NULL;

    if ( battery->priv->type == UP_DEVICE_KIND_BATTERY ||
         battery->priv->type == UP_DEVICE_KIND_UPS )
    {
        power_status = g_strdup_printf (battery->priv->ac_online ? _("Adaptor is online") : _("System is running on battery power"));
    }
    else
    {
	/* Let the user know it's not a system battery being monitored */
        power_status = g_strdup(_("Peripheral Device"));
    }

    if ( battery->priv->state == UP_DEVICE_STATE_FULLY_CHARGED )
    {
	if ( battery->priv->time_to_empty > 0 )
	{
	    est_time_str = xfpm_battery_get_time_string (battery->priv->time_to_empty);
	    tip = g_strdup_printf (_("%s\nYour %s is fully charged (%i%%).\nProvides %s runtime"),
				   power_status,
				   battery->priv->battery_name,
				   battery->priv->percentage,
				   est_time_str);
	    g_free (est_time_str);
	}
	else
	{
	    tip = g_strdup_printf (_("%s\nYour %s is fully charged (%i%%)."),
				   power_status,
				   battery->priv->battery_name,
				   battery->priv->percentage);
	}
    }
    else if ( battery->priv->state == UP_DEVICE_STATE_CHARGING )
    {
	if ( battery->priv->time_to_full != 0 )
	{
	    est_time_str = xfpm_battery_get_time_string (battery->priv->time_to_full);
	    tip = g_strdup_printf (_("%s\nYour %s is charging (%i%%)\n%s until is fully charged."),
				   power_status,
				   battery->priv->battery_name,
				   battery->priv->percentage,
				   est_time_str);
	    g_free (est_time_str);
	}
	else
	{
	    tip = g_strdup_printf (_("%s\nYour %s is charging (%i%%)."),
				   power_status,
				   battery->priv->battery_name,
				   battery->priv->percentage);
	}
    }
    else if ( battery->priv->state == UP_DEVICE_STATE_DISCHARGING )
    {
	if ( battery->priv->time_to_empty != 0 )
	{
	    est_time_str = xfpm_battery_get_time_string (battery->priv->time_to_empty);
	    tip = g_strdup_printf (_("%s\nYour %s is discharging (%i%%)\nEstimated time left is %s."),
				   power_status,
				   battery->priv->battery_name,
				   battery->priv->percentage,
				   est_time_str);
	    g_free (est_time_str);
	}
	else
	{
	    tip = g_strdup_printf (_("%s\nYour %s is discharging (%i%%)."),
				   power_status,
				   battery->priv->battery_name,
				   battery->priv->percentage);
	}

    }
    else if ( battery->priv->state == UP_DEVICE_STATE_PENDING_CHARGE )
    {
	tip = g_strdup_printf (_("%s\n%s waiting to discharge (%i%%)."), power_status, battery->priv->battery_name, battery->priv->percentage);
    }
    else if ( battery->priv->state == UP_DEVICE_STATE_PENDING_DISCHARGE )
    {
	tip = g_strdup_printf (_("%s\n%s waiting to charge (%i%%)."), power_status, battery->priv->battery_name, battery->priv->percentage);
    }
    else if ( battery->priv->state == UP_DEVICE_STATE_EMPTY )
    {
	tip = g_strdup_printf (_("%s\nYour %s is empty"), power_status, battery->priv->battery_name);
    }
    else
    {
	/* unknown device state, just display the percentage */
	tip = g_strdup_printf (_("%s\nYour %s is at (%i%%)."),
				   power_status,
				   battery->priv->battery_name,
				   battery->priv->percentage);
    }

    gtk_tooltip_set_text (tooltip, tip);
    g_free (power_status);
    g_free (tip);
}

static void
xfpm_battery_check_charge (XfpmBattery *battery)
{
    XfpmBatteryCharge charge;
    guint critical_level, low_level;

    g_object_get (G_OBJECT (battery->priv->conf),
		  CRITICAL_POWER_LEVEL, &critical_level,
		  NULL);

    low_level = critical_level + 10;

    if ( battery->priv->percentage > low_level )
	charge = XFPM_BATTERY_CHARGE_OK;
    else if ( battery->priv->percentage <= low_level && battery->priv->percentage > critical_level )
	charge = XFPM_BATTERY_CHARGE_LOW;
    else if ( battery->priv->percentage <= critical_level )
	charge = XFPM_BATTERY_CHARGE_CRITICAL;
    else
	charge = XFPM_BATTERY_CHARGE_UNKNOWN;

    if ( charge != battery->priv->charge)
    {
	battery->priv->charge = charge;
	/*
	 * only emit signal when when battery charge changes from ok->low->critical
	 * and not the other way round.
	 */
	if ( battery->priv->charge != XFPM_BATTERY_CHARGE_CRITICAL || charge != XFPM_BATTERY_CHARGE_LOW )
	    g_signal_emit (G_OBJECT (battery), signals [BATTERY_CHARGE_CHANGED], 0);
    }
}

static void
xfpm_battery_refresh (XfpmBattery *battery, UpDevice *device)
{
    gboolean present;
    guint state;
    gdouble percentage;
    guint64 to_empty, to_full;
    g_object_get(device,
		 "is-present", &present,
		 "percentage", &percentage,
		 "state", &state,
		 "time-to-empty", &to_empty,
		 "time-to-full", &to_full,
		 NULL);

    battery->priv->present = present;
    if ( state != battery->priv->state )
    {
	battery->priv->state = state;
	xfpm_battery_refresh_visible (battery);
	xfpm_battery_notify_state (battery);
    }
    battery->priv->percentage = (guint) percentage;

    xfpm_battery_check_charge (battery);

    xfpm_battery_refresh_icon (battery);

    if ( battery->priv->type == UP_DEVICE_KIND_BATTERY ||
	 battery->priv->type == UP_DEVICE_KIND_UPS )
    {
	battery->priv->time_to_empty = to_empty;
	battery->priv->time_to_full  = to_empty;
    }
}

static void
xfpm_battery_button_pressed_cb (XfpmButton *button, XfpmButtonKey type, XfpmBattery *battery)
{
    if (type == BUTTON_BATTERY)
	xfpm_battery_notify (battery);
}

static void
xfpm_battery_changed_cb (UpDevice *device,
#if UP_CHECK_VERSION(0, 99, 0)
			 GParamSpec *pspec,
#endif
			 XfpmBattery *battery)
{
    xfpm_battery_refresh (battery, device);
}

static gboolean
xfpm_battery_query_tooltip (GtkStatusIcon *icon,
			    gint x,
			    gint y,
			    gboolean keyboard_mode,
			    GtkTooltip *tooltip)
{
    XfpmBattery *battery;

    battery = XFPM_BATTERY (icon);

    xfpm_battery_set_tooltip_primary (battery, tooltip);
    return TRUE;
}

static void
xfpm_battery_tray_icon_settings_changed (GObject *obj, GParamSpec *spec, XfpmBattery *battery)
{
    xfpm_battery_refresh_visible (battery);
}

static void xfpm_battery_get_property (GObject *object,
				       guint prop_id,
				       GValue *value,
				       GParamSpec *pspec)
{
    XfpmBattery *battery;

    battery = XFPM_BATTERY (object);

    switch (prop_id)
    {
	case PROP_AC_ONLINE:
	    g_value_set_boolean (value, battery->priv->ac_online);
	    break;
	case PROP_DEVICE_TYPE:
	    g_value_set_enum (value, battery->priv->type);
	    break;
	case PROP_CHARGE_STATUS:
	    g_value_set_enum (value, battery->priv->charge);
	    break;
	default:
	    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

static void xfpm_battery_set_property (GObject *object,
				       guint prop_id,
				       const GValue *value,
				       GParamSpec *pspec)
{
    XfpmBattery *battery;

    battery = XFPM_BATTERY (object);

    switch (prop_id)
    {
	case PROP_AC_ONLINE:
	    battery->priv->ac_online = g_value_get_boolean (value);
	    xfpm_battery_refresh_icon (battery);
	    break;
	default:
	    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}


static void
xfpm_battery_class_init (XfpmBatteryClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    GtkStatusIconClass *status_icon_class = GTK_STATUS_ICON_CLASS (klass);

    object_class->finalize = xfpm_battery_finalize;
    object_class->get_property = xfpm_battery_get_property;
    object_class->set_property = xfpm_battery_set_property;

    status_icon_class->query_tooltip = xfpm_battery_query_tooltip;

    signals [BATTERY_CHARGE_CHANGED] =
        g_signal_new ("battery-charge-changed",
                      XFPM_TYPE_BATTERY,
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET(XfpmBatteryClass, battery_charge_changed),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0, G_TYPE_NONE);

    g_object_class_install_property (object_class,
                                     PROP_AC_ONLINE,
                                     g_param_spec_boolean("ac-online",
                                                          NULL, NULL,
                                                          FALSE,
                                                          G_PARAM_READWRITE));

    g_object_class_install_property (object_class,
                                     PROP_DEVICE_TYPE,
                                     g_param_spec_uint ("device-type",
                                                        NULL, NULL,
							UP_DEVICE_KIND_UNKNOWN,
							UP_DEVICE_KIND_LAST,
							UP_DEVICE_KIND_UNKNOWN,
                                                        G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_CHARGE_STATUS,
                                     g_param_spec_enum ("charge-status",
                                                        NULL, NULL,
							XFPM_TYPE_BATTERY_CHARGE,
							XFPM_BATTERY_CHARGE_UNKNOWN,
                                                        G_PARAM_READABLE));

    g_type_class_add_private (klass, sizeof (XfpmBatteryPrivate));
}

static void
xfpm_battery_init (XfpmBattery *battery)
{
    battery->priv = XFPM_BATTERY_GET_PRIVATE (battery);

    battery->priv->conf          = xfpm_xfconf_new ();
    battery->priv->notify        = xfpm_notify_new ();
    battery->priv->device        = NULL;
    battery->priv->client        = NULL;
    battery->priv->state         = UP_DEVICE_STATE_UNKNOWN;
    battery->priv->type          = UP_DEVICE_KIND_UNKNOWN;
    battery->priv->charge        = XFPM_BATTERY_CHARGE_UNKNOWN;
    battery->priv->icon_prefix   = NULL;
    battery->priv->time_to_full  = 0;
    battery->priv->time_to_empty = 0;
    battery->priv->button        = xfpm_button_new ();
    battery->priv->ac_online     = TRUE;

    battery->priv->sig = g_signal_connect (G_OBJECT (battery->priv->conf), "notify::" SHOW_TRAY_ICON_CFG,
					   G_CALLBACK (xfpm_battery_tray_icon_settings_changed), battery);


    battery->priv->sig_bt = g_signal_connect (G_OBJECT (battery->priv->button), "button-pressed",
					   G_CALLBACK (xfpm_battery_button_pressed_cb), battery);
}

static void
xfpm_battery_finalize (GObject *object)
{
    XfpmBattery *battery;

    battery = XFPM_BATTERY (object);

    g_free (battery->priv->icon_prefix);

    if (battery->priv->notify_idle != 0)
        g_source_remove (battery->priv->notify_idle);

    if ( g_signal_handler_is_connected (battery->priv->device, battery->priv->sig_up ) )
	g_signal_handler_disconnect (G_OBJECT (battery->priv->device), battery->priv->sig_up);

    if ( g_signal_handler_is_connected (battery->priv->conf, battery->priv->sig ) )
	g_signal_handler_disconnect (G_OBJECT (battery->priv->conf), battery->priv->sig);

     if ( g_signal_handler_is_connected (battery->priv->button, battery->priv->sig_bt ) )
	g_signal_handler_disconnect (G_OBJECT (battery->priv->button), battery->priv->sig_bt);

    g_object_unref (battery->priv->device);
    g_object_unref (battery->priv->conf);
    g_object_unref (battery->priv->notify);
    g_object_unref (battery->priv->button);

    gtk_status_icon_set_visible(GTK_STATUS_ICON(battery), FALSE);

    G_OBJECT_CLASS (xfpm_battery_parent_class)->finalize (object);
}

static gchar *
xfpm_battery_get_icon_prefix_device_enum_type (UpDeviceKind type)
{
    if ( type == UP_DEVICE_KIND_BATTERY )
    {
	return g_strdup (XFPM_PRIMARY_ICON_PREFIX);
    }
    else if ( type == UP_DEVICE_KIND_UPS )
    {
	return g_strdup (XFPM_UPS_ICON_PREFIX);
    }
    else if ( type == UP_DEVICE_KIND_MOUSE )
    {
	return g_strdup (XFPM_MOUSE_ICON_PREFIX);
    }
    else if ( type == UP_DEVICE_KIND_KEYBOARD )
    {
	return g_strdup (XFPM_KBD_ICON_PREFIX);
    }
    else if ( type == UP_DEVICE_KIND_PHONE )
    {
	return g_strdup (XFPM_PHONE_ICON_PREFIX);
    }

    return g_strdup (XFPM_PRIMARY_ICON_PREFIX);
}

static const gchar *
xfpm_battery_get_name (UpDeviceKind type)
{
    const gchar *name = NULL;

    switch (type)
    {
	case UP_DEVICE_KIND_BATTERY:
	    name = _("battery");
	    break;
	case UP_DEVICE_KIND_UPS:
	    name = _("UPS");
	    break;
	case UP_DEVICE_KIND_MONITOR:
	    name = _("monitor battery");
	    break;
	case UP_DEVICE_KIND_MOUSE:
	    name = _("mouse battery");
	    break;
	case UP_DEVICE_KIND_KEYBOARD:
	    name = _("keyboard battery");
	    break;
	case UP_DEVICE_KIND_PDA:
	    name = _("PDA battery");
	    break;
	case UP_DEVICE_KIND_PHONE:
	    name = _("Phone battery");
	    break;
	default:
	    name = _("Unknown");
	    break;
    }

    return name;
}

GtkStatusIcon *
xfpm_battery_new (void)
{
    XfpmBattery *battery = NULL;

    battery = g_object_new (XFPM_TYPE_BATTERY, NULL);

    return GTK_STATUS_ICON (battery);
}

void xfpm_battery_monitor_device (XfpmBattery *battery,
				  const char *object_path,
				  UpDeviceKind device_type)
{
    UpDevice *device;
    battery->priv->type = device_type;
    battery->priv->client = up_client_new();
    battery->priv->icon_prefix = xfpm_battery_get_icon_prefix_device_enum_type (device_type);
    battery->priv->battery_name = xfpm_battery_get_name (device_type);

    device = up_device_new();
    up_device_set_object_path_sync (device, object_path, NULL, NULL);
    battery->priv->device = device;
#if UP_CHECK_VERSION(0, 99, 0)
    battery->priv->sig_up = g_signal_connect (battery->priv->device, "notify", G_CALLBACK (xfpm_battery_changed_cb), battery);
#else
    battery->priv->sig_up = g_signal_connect (battery->priv->device, "changed", G_CALLBACK (xfpm_battery_changed_cb), battery);
#endif
    g_object_set (G_OBJECT (battery),
		  "has-tooltip", TRUE,
		  NULL);

    xfpm_battery_refresh (battery, device);
}

UpDeviceKind xfpm_battery_get_device_type (XfpmBattery *battery)
{
    g_return_val_if_fail (XFPM_IS_BATTERY (battery), UP_DEVICE_KIND_UNKNOWN );

    return battery->priv->type;
}

XfpmBatteryCharge xfpm_battery_get_charge (XfpmBattery *battery)
{
    g_return_val_if_fail (XFPM_IS_BATTERY (battery), XFPM_BATTERY_CHARGE_UNKNOWN);

    return battery->priv->charge;
}

const gchar *xfpm_battery_get_battery_name (XfpmBattery *battery)
{
    g_return_val_if_fail (XFPM_IS_BATTERY (battery), NULL);

    return battery->priv->battery_name;
}

gchar *xfpm_battery_get_time_left (XfpmBattery *battery)
{
    g_return_val_if_fail (XFPM_IS_BATTERY (battery), NULL);

    return xfpm_battery_get_time_string (battery->priv->time_to_empty);
}
