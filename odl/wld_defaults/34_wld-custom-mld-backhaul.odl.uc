%populate {
    object WiFi {
        object 'SSID' {
{% let lastapindex = 0 %}
{% for ( let Itf in BD.Interfaces ) : if ( BDfn.isInterfaceWirelessAp(Itf.Name) ) : %}
    {% lastapindex++ %}
{% endif; endfor; %}
{% if (lastapindex == 0) : %}
    {% lastapindex = 8 %}
{% endif %}
{% for ( let Radio in BD.Radios ) : %}
            object 'backhaul_{{Radio.Alias}}' {
{% if (Radio.OperatingFrequency != "2.4GHz") : %}
                parameter SSID = "Backhaul_SSID";
                parameter MLDUnit = {{lastapindex}};
{% else %}
                parameter MLDUnit = -1;
{% endif %}
            }
{% endfor %}
        }
        object AccessPoint {
{% for ( let Radio in BD.Radios ) : %}
{% if (Radio.OperatingFrequency != "2.4GHz") : %}
            object 'backhaul_{{Radio.Alias}}' {
                parameter Enable = 1;
                object Security {
                    parameter ModeEnabled = "WPA3-Personal";
                    parameter SAEPassphrase = "password";
                }
            }
{% endif %}
{% endfor %}
        }
    }
}