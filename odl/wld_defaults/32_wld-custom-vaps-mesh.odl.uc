%populate {
    object WiFi {
        object AccessPoint {
{% for ( let Itf in BD.Interfaces ) : if ( BDfn.isInterfaceWirelessAp(Itf.Name) ) : %}
            object '{{Itf.Alias}}' {
                parameter MBOEnable = true;
                parameter UAPSDEnable = true;
                object IEEE80211u {
                    parameter InterworkingEnable = 1;
                }
                object Vendor {
                    parameter EnableBssLoad = true;
                }
{% if (!(BDfn.isInterfaceGuest(Itf.Name))) : %}
                object WPS {
                    {% if ( Itf.OperatingFrequency != "6GHz" ) : %}
                                parameter Enable = true;
                    {% endif %}
                    parameter ConfigMethodsEnabled = "PhysicalPushButton,VirtualPushButton,VirtualDisplay,PIN";
                }
{% endif %}
{% if ( Itf.OperatingFrequency == "6GHz" ) : %}
                object Security {
                    parameter SAEPassphrase = "password";
                }
{% endif %}
            }
{% endif; endfor; %}
        }
    }
}
