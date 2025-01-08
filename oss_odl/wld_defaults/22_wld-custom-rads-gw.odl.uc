%populate {
    object WiFi {
        object Radio {
{% for ( let Radio in BD.Radios ) : %}
            object '{{Radio.Alias}}' {
                parameter RegulatoryDomain = "US";
                parameter DTIMPeriod = 2;
                parameter MultiUserMIMOEnabled = 1;
                parameter ActiveAntennaCtrl = 15;
                parameter TxChainCtrl = 15;
                parameter RxChainCtrl = 15;
            }
{% if (Radio.OperatingFrequency == "5GHz") : %}
            object '{{Radio.Alias}}' {
                parameter STA_Mode = false;
                parameter STASupported_Mode = false;
                parameter AutoChannelEnable = true;
                parameter OperatingChannelBandwidth = "80MHz";
                object IEEE80211ax {
                    parameter MBSSIDAdvertisementMode = "Off";
                }
                object ChannelMgt {
                    object BgDfs {
                        parameter PreclearEnable = false;
                    }
                }
            }
{% elif (Radio.OperatingFrequency == "6GHz") : %}
            object '{{Radio.Alias}}' {
                parameter Channel = 37;
                parameter OperatingChannelBandwidth = "160MHz";
                object IEEE80211ax {
                    parameter MBSSIDAdvertisementMode = "On";
                }
            }
{% elif (Radio.OperatingFrequency == "2.4GHz") : %}
            object '{{Radio.Alias}}' {
                parameter AutoChannelEnable = true;
                parameter OperatingChannelBandwidth = "20MHz";
                parameter ObssCoexistenceEnable = true;
                object IEEE80211ax {
                    parameter MBSSIDAdvertisementMode = "Off";
                }
            }
{% endif; endfor; %}
        }
    }
}
