%populate {
    object WiFi {
        object 'SSID' {
{% for ( let Itf in BD.Interfaces ) : if ( BDfn.isInterfaceWirelessAp(Itf.Name) ) : %}
            object '{{Itf.Alias}}' {
{% if (Itf.MLDUnit != null && Itf.MLDUnit != undefined && int(Itf.MLDUnit) > -1 ) : %}
                parameter MLDUnit = {{Itf.MLDUnit}};
{% else %}
                parameter MLDUnit = -1;
{% endif %}
            }
{% endif; endfor; %}
        }
        object AccessPoint {
{% for ( let Itf in BD.Interfaces ) : if ( BDfn.isInterfaceWirelessAp(Itf.Name) ) : %}
{% if (Itf.MLDUnit != null && Itf.MLDUnit != undefined && int(Itf.MLDUnit) > -1 ) : %}
            object '{{Itf.Alias}}' {
                object Security {
                    parameter ModeEnabled = "WPA3-Personal";
                    parameter SAEPassphrase = "password";
                }
            }
{% endif %}
{% endif; endfor; %}
        }
    }
}