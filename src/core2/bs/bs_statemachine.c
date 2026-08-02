// BanjoDecomp: core2/code_13780.c
#include <ultra64.h>
#include "functions.h"
#include "variables.h"

#include "bsint.h"

// [port] VR first person: face Banjo where the player is looking before an aim state initializes.
extern void port_vrFpFaceViewYaw(void);
// [port] Freshness stamp: bs_getState() is a raw global nothing clears when gameplay ends, so
// consumers outside the player tick are reading a corpse (a drowned save carried BS_54_SWIM_DIE
// all the way to the file select screen). Stamped every live state-machine tick.
extern void port_vrBsTicked(void);

s32 D_8037D160; //prev_state
s32 D_8037D164; //state
s32 D_8037D168; //next_state
s32 D_8037D16C; 
s32 D_8037D170;

void bs_clearState(void){
    D_8037D160 = 0;
    D_8037D164 = 0;
    D_8037D168 = 0;
}

void bs_setState(s32 state_id){

    if(state_id == 0)
        return;
    
    D_8037D168 = state_id;
    if(bsList_getEndMethod(D_8037D164) != NULL)
        bsList_getEndMethod(D_8037D164)();

    
    D_8037D160 = D_8037D164;
    D_8037D164 = D_8037D168;
    D_8037D168 = 0;

    // [port] VR first person: the right stick turns the VIEW base, not Banjo, so a body-facing
    // attack could launch at whatever direction the bear last walked - invisible from inside his
    // head. When an aim-driven state begins, face him where the player is LOOKING first, so
    // barges, swipes, pecks and eggs go where the eyes point. No-op outside VR first person, and
    // it runs BEFORE the state's init so the init reads the aligned yaw.
    switch (D_8037D164) {
        case BS_13_BBARGE:
        case BS_6_CLAW:
        case BS_11_BPECK:
        case BS_9_EGG_HEAD:
        case BS_A_EGG_ASS:
            port_vrFpFaceViewYaw();
            break;
    }

    if(bsList_getInitMethod(D_8037D164) != NULL)
        bsList_getInitMethod(D_8037D164)();
}

s32 bs_getPrevState(void){
    return D_8037D160;
}

s32 bs_getState(void){
    return D_8037D164;
}

s32 bs_getNextState(void){
    return D_8037D168;
}

void bs_updateState(void){
    port_vrBsTicked(); // [port] the only proof a live player is behind bs_getState()
    if(bsList_getUpdateMethod(D_8037D164) != NULL)
        bsList_getUpdateMethod(D_8037D164)();
}

s32 bs_checkInterrupt(enum bs_interrupt_e arg0){
    D_8037D16C = arg0;
    D_8037D170 = 0;
    if(bsList_getInterruptMethod(D_8037D164) != NULL)
        bsList_getInterruptMethod(D_8037D164)();
    return D_8037D170;
}

void bs_setInterruptResponse(s32 arg0){
    D_8037D170 = arg0;
}

enum bs_interrupt_e bs_getInterruptType(void){
    return D_8037D16C;
}
