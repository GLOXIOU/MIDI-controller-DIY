| title | MIDI-controller-DIY |
| --- | --- |
| author | GLOXIOU |
| description | A DIY project for a MIDI controller designed to control lights, featuring knobs, faders, and buttons. |
| created_at | 2026-09-23 |

# Day 1: Defining system & idea search & inspiration

I want to make a MIDI controlleur to control my lights via my already made [DMX Unit](https://github.com/GLOXIOU/DMX-Unit-DIY), with 9 faders, 1 master fader, for all of the faders, 2 keyboards low profile key, and 2 potentiometer. I also want 2 larger rotary encoders with push-button function. All of that in a box, designed in 3D in Fusion 360, and for the first time in my life with a PCB. All of that will be managed by an ESP32-S3, and I want just a USB-C output for the MIDI.

My inspiration is the GrandMa, wich cost around 7000$. I want to make that, but for much cheaper, and with just the functionality I need. The only thing I want to keep from that housing is the utility.

![Image 1](images/1.jpg)

So I started by making a litlle schema of what I want and what I imagine:

![Image 2](images/2.jpg)

Why 9 faders and 1 master ? Beacause on Aliexpress, tehre's only lots of 10 pieces. But that might changes, like maybe I will add 5 or I don't know. For the parts, I check online and talk to some friends, and apprenntly, this is the best:

* 100mm - 6 Pins - 10K ohms x10
* EC11 rotary encoder x3 (I need to make the large wheel above the potentiometer in 3D)
* WH148 10K omhs x10
* Kailh 1350 Choc V1 - Brown switch x10

For a price arround 50 dollars with the PCB I believe, but it's need to be confirmed. The last time I order on aliexpress, I payed duble the price beacause of the importation tax in europe, so I need to be carefull this time...

Nothing is finished yet, but here is where the project stands at this point. Now that I have all this, I think the first step will be to design the PCB, so I can then create the 3D enclosure and finally work on the code.

**Total time spent: 1.5 hours**