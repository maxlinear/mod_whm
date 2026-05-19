%populate {
    object WiFi {
        object 'SSID' {
{% for ( let Radio in BD.Radios ) : %}
            object 'backhaul_{{Radio.Alias}}' {
                parameter MLDUnit = -1;
            }
{% endfor %}
        }
    }
}