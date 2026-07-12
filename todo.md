# TODO

based on firmware artifacts created by 8a60289ccf75f41b138ae7096aeb534f37675cdb at 13-jul-2026-0300 SGT

## Bugs

- [ ] rotary encoder, figure out why LRscroll is not working
- [ ] dpad scroll on main layer still kinda too fast for my taste
        - wrt excel, dpad scroll on tap goes from cell A to Q
            - target scroll distance should be cell E
            - suspected to be due to debouncing
        - same for keyboard scroll controls on layer 2, keyboard press delay should be changed
            - `trigger-period-ms = <200>;         // default: 16 -- 4x tick rate to slow scroll reports
        - balance debouncing issue to make sure ticks fire off at diff rates from normal keypresses for scroll movement
        - for better reference refer to redox configuration on vial
        <!-- - dump stats here  -->

## oled display

- [ ] add kronii illust, literally change the imagery thats all
    - [ ] can try  <https://github.com/whoop-t/nice-shield-base> i guess
    - [ ] try to get a hololive pic in 128x32 vertical  
            - <https://javl.github.io/image2cpp/>
            - can try to put BT and BAT status in her ring

## RGB lighting

- [ ] add lighting for each layer

## keymaps

- [ ] add macro layer, figure out a button to access this
        - can try toggle layer buttons to a macro pad that always returns to main
- [ ] add controller mode
- [ ] add alt key on thumb cluster for Rhold utils layer
