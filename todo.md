# TODO

based on firmware artifacts created by 8a60289ccf75f41b138ae7096aeb534f37675cdb at 13-jul-2026-0300 SGT

## Bugs

- [ ] scroll using keypressing still experiences some lag, theres a delay between initial press and expected scroll time
            - `trigger-period-ms = <200>;         // default: 16 -- 4x tick rate to slow scroll reports

## oled display

- [ ] add kronii illust, literally change the imagery thats all
    - [ ] can try  <https://github.com/whoop-t/nice-shield-base> i guess
    - [ ] try to get a hololive pic in 128x32 vertical  
            - <https://javl.github.io/image2cpp/>
            - can try to put BT and BAT status in her ring
- [ ] add bad apple idle video

## RGB lighting

- [ ] add lighting for each layer
- [ ] add bad apple idle video


## Connectivity

- [ ] Add RFC connectivity
      - apparently nicenanos have RFC compatability in built, its just that the SDK is bespoke
      - maybe we could make one and merge to the ZMK main repo and provide support
