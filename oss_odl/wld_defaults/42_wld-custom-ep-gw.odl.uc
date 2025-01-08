%populate {
    object WiFi {
{% let BssId = 0 %}
{% let BssId6g = 0 %}
{% for ( let Itf in BD.Interfaces ) : if ( Itf.Type == "wireless" ) : %}
{% BssId++ %}
{% BssId6g++ %}
{% endif; endfor; %}
{% RadioIndex = BDfn.getRadioIndex("2.4GHz"); if (RadioIndex >= 0) : %}
{% BssId = BssId + 2 %}
{% endif %}
{% RadioIndex6g = BDfn.getRadioIndex("6GHz"); if (RadioIndex6g >= 0) : %}
{% BssId6g = BssId6g + 3 %}
{% endif %}
    }
}
