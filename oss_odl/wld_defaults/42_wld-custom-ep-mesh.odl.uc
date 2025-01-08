%populate {
    object WiFi {
{% let BssId = 0 %}
{% for ( let Itf in BD.Interfaces ) : if ( Itf.Type == "wireless" ) : %}
{% BssId++ %}
{% endif; endfor; %}
{% RadioIndex = BDfn.getRadioIndex("2.4GHz"); if (RadioIndex >= 0) : %}
{% BssId = BssId + 2 %}
{% endif %}
    }
}
