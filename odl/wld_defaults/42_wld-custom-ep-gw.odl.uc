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
        object SSID {
            instance add ({{BssId}}, "ep2g0") {
                parameter LowerLayers = "Device.WiFi.Radio.{{RadioIndex + 1}}.";
            }
            instance add ({{BssId6g}}, "ep6g0") {
                parameter LowerLayers = "Device.WiFi.Radio.{{RadioIndex6g + 1}}.";
            }
        }
        object EndPoint {
            instance add ("ep2g0") {
                parameter SSIDReference = "Device.WiFi.SSID.{{BssId}}.";
                parameter Enable = 0;
                parameter BridgeInterface = "{{BD.Bridges.Lan.Name}}";
                parameter MultiAPEnable = 1;
                object WPS {
                    parameter Enable = 1;
                    parameter ConfigMethodsEnabled = "PhysicalPushButton,VirtualPushButton,VirtualDisplay,PIN";
                }
            }
            instance add ("ep6g0") {
                parameter SSIDReference = "Device.WiFi.SSID.{{BssId6g}}.";
                parameter Enable = 0;
                parameter BridgeInterface = "{{BD.Bridges.Lan.Name}}";
                parameter MultiAPEnable = 1;
                object WPS {
                    parameter Enable = 1;
                    parameter ConfigMethodsEnabled = "PhysicalPushButton,VirtualPushButton,VirtualDisplay,PIN";
                }
            }
        }
    }
}