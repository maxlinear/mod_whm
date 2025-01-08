%populate {
    object WiFi {
{% let BssId = 0 %}
{% for ( let Itf in BD.Interfaces ) : if ( Itf.Type == "wireless" ) : %}
{% BssId++ %}
{% endif; endfor; %}
{% RadioIndex = BDfn.getRadioIndex("2.4GHz"); if (RadioIndex >= 0) : %}
{% BssId = BssId + 2 %}
        object SSID {
            instance add ({{BssId}}, "ep2g0") {
                parameter LowerLayers = "Device.WiFi.Radio.{{RadioIndex + 1}}.";
            }
        }
        object EndPoint {
            object 'ep5g0' {
                parameter Enable = 1;
            }
            instance add ("ep2g0") {
                parameter SSIDReference = "Device.WiFi.SSID.{{BssId}}.";
                parameter Enable = 1;
                parameter BridgeInterface = "{{BD.Bridges.Lan.Name}}";
                parameter MultiAPEnable = 1;
                object WPS {
                    parameter Enable = 1;
                    parameter ConfigMethodsEnabled = "PhysicalPushButton,VirtualPushButton,VirtualDisplay,PIN";
                }
            }
        }
{% endif %}
    }
}
