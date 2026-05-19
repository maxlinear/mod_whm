%populate {
    object WiFi {
        object 'SSID' {
{% for ( let Itf in BD.Interfaces ) : if ( BDfn.isInterfaceWirelessAp(Itf.Name) ) : %}
            object '{{Itf.Alias}}' {
                parameter MLDUnit = -1;
            }
{% endif; endfor; %}
        }
    }
}