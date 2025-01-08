%populate {
    object WiFi {
        object AccessPoint {
{% for ( let Itf in BD.Interfaces ) : if ( BDfn.isInterfaceWirelessAp(Itf.Name) ) : %}
            object '{{Itf.Alias}}' {
                parameter Enable = true;
                parameter MBOEnable = true;
                parameter MultiAPType = "FronthaulBSS";
                parameter UAPSDEnable = true;
                object IEEE80211u {
                    parameter InterworkingEnable = 1;
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
                    parameter ModesAvailable = "WPA3-Personal,OWE";
                    parameter SAEPassphrase = "password";
                    parameter MFPConfig = "Required";
                }
{% else %}
                object Security {
                    parameter ModesAvailable = "None,WPA2-Personal,WPA3-Personal,WPA2-WPA3-Personal,WPA2-Enterprise,OWE";
                }

{% endif %}
                object Security {
                    parameter RekeyingInterval = 86400;
                }
            }
{% endif; endfor; %}
        }
    }
}
