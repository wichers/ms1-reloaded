/* CPU core tests for what the differential test cannot reach: exception entry and return, RTI
 * chaining into a pending interrupt, the one-instruction delay after clearing I, WAI, SWI,
 * TRAP, CALL/RTC, and the X bit rules. Expected values follow the S12CPUV2 Reference Manual. */
#include <stdio.h>
#include <string.h>

#include "../src/cpu12.h"

static uint8_t mem[65536];
static uint16_t pending;   /* vector of the pending interrupt, 0 none */
static uint8_t ppage;
static int failures;

static uint8_t rd(void *c, uint16_t a) { (void)c; return mem[a]; }
static void wr(void *c, uint16_t a, uint8_t v) { (void)c; mem[a] = v; }
static uint16_t irq(void *c) { (void)c; return pending; }
static uint8_t gp(void *c) { (void)c; return ppage; }
static void sp_(void *c, uint8_t p) { (void)c; ppage = p; }

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void setup(cpu12_t *c, uint16_t pc)
{
	memset(mem, 0, sizeof mem);
	memset(c, 0, sizeof *c);
	c->bus = (cpu12_bus_t){ 0, rd, wr, irq, 0, gp, sp_ };
	mem[0xFFFE] = (uint8_t)(pc >> 8);
	mem[0xFFFF] = (uint8_t)pc;
	cpu12_reset(c, 0xFFFE);
	c->sp = 0x2000;
	pending = 0;
}

static void vector(uint16_t v, uint16_t target)
{
	mem[v] = (uint8_t)(target >> 8);
	mem[v + 1] = (uint8_t)target;
}

static void put(uint16_t a, const uint8_t *b, int n) { memcpy(&mem[a], b, (size_t)n); }

static void test_interrupt_frame(void)
{
	cpu12_t c;
	setup(&c, 0x4000);
	c.a = 0x11; c.b = 0x22; c.x = 0x3344; c.y = 0x5566; c.ccr = 0x00;
	vector(0xFFEE, 0x5000);
	mem[0x4000] = 0xA7;                       /* NOP */
	mem[0x5000] = 0x0B;                       /* RTI */
	pending = 0xFFEE;
	int cyc = cpu12_step(&c);
	CHECK(cyc == 9, "interrupt entry takes 9 cycles, got %d", cyc);
	CHECK(c.pc == 0x5000 && (c.ccr & CCR_I), "vectored with I set");
	CHECK(c.sp == 0x2000 - 9, "9 bytes stacked, SP=%04X", c.sp);
	CHECK(mem[c.sp] == 0x00 && mem[c.sp + 1] == 0x22 && mem[c.sp + 2] == 0x11, "CCR, B, A order");
	CHECK(mem[c.sp + 3] == 0x33 && mem[c.sp + 5] == 0x55 && mem[c.sp + 7] == 0x40 && mem[c.sp + 8] == 0x00,
		"X, Y, return address");
	pending = 0;
	cyc = cpu12_step(&c);
	CHECK(cyc == 8 && c.pc == 0x4000 && c.sp == 0x2000 && c.a == 0x11 && c.b == 0x22 && c.ccr == 0, "RTI restores");
}

static void test_rti_chain(void)
{
	cpu12_t c;
	setup(&c, 0x4000);
	c.ccr = 0;
	vector(0xFFEE, 0x5000);
	vector(0xFFEC, 0x6000);
	mem[0x4000] = 0xA7;
	mem[0x5000] = 0x0B;
	mem[0x6000] = 0x0B;
	pending = 0xFFEE;
	cpu12_step(&c);                           /* enter ch0 handler */
	pending = 0xFFEC;                         /* another source pending when RTI runs */
	int cyc = cpu12_step(&c);                 /* RTI */
	cyc += cpu12_step(&c);                    /* chained entry */
	CHECK(cyc == 11, "RTI + chained entry = 11 cycles, got %d", cyc);
	CHECK(c.pc == 0x6000 && c.sp == 0x2000 - 9, "second handler entered on the same frame, SP=%04X", c.sp);
	pending = 0;
	cpu12_step(&c);
	CHECK(c.pc == 0x4000 && c.sp == 0x2000, "returns to the interrupted code, PC=%04X SP=%04X", c.pc, c.sp);
}

static void test_cli_delay(void)
{
	cpu12_t c;
	setup(&c, 0x4000);
	vector(0xFFEE, 0x5000);
	const uint8_t prog[] = { 0x10, 0xEF, 0xA7, 0xA7 };   /* CLI; NOP; NOP */
	put(0x4000, prog, 4);
	pending = 0xFFEE;
	cpu12_step(&c);                           /* CLI (reset left I set) */
	cpu12_step(&c);                           /* the next instruction still runs */
	CHECK(c.pc == 0x4003, "instruction after CLI executes, PC=%04X", c.pc);
	cpu12_step(&c);
	CHECK(c.pc == 0x5000, "then the interrupt, PC=%04X", c.pc);
}

static void test_wai(void)
{
	cpu12_t c;
	setup(&c, 0x4000);
	c.ccr = 0;
	vector(0xFFEE, 0x5000);
	mem[0x4000] = 0x3E;                       /* WAI */
	cpu12_step(&c);
	CHECK(c.state == CPU12_WAIT && c.sp == 0x2000 - 9, "WAI stacks and waits");
	CHECK(cpu12_step(&c) == 0, "nothing to do while waiting");
	pending = 0xFFEE;
	int cyc = cpu12_step(&c);
	CHECK(c.pc == 0x5000 && c.sp == 0x2000 - 9 && cyc == 5, "wakes without re-stacking (%d cycles)", cyc);
	CHECK(mem[c.sp + 7] == 0x40 && mem[c.sp + 8] == 0x01, "return address after WAI");
}

static void test_swi_trap(void)
{
	cpu12_t c;
	setup(&c, 0x4000);
	vector(0xFFF6, 0x5000);
	vector(0xFFF8, 0x6000);
	mem[0x4000] = 0x3F;                       /* SWI */
	cpu12_step(&c);
	CHECK(c.pc == 0x5000 && mem[c.sp + 7] == 0x40 && mem[c.sp + 8] == 0x01, "SWI returns after itself");
	setup(&c, 0x4000);
	vector(0xFFF8, 0x6000);
	mem[0x4000] = 0x18;
	mem[0x4001] = 0x30;                       /* TRAP $30 */
	int cyc = cpu12_step(&c);
	CHECK(c.pc == 0x6000 && mem[c.sp + 8] == 0x02 && cyc == 10, "TRAP: vector $FFF8, returns after it");
}

static void test_call_rtc(void)
{
	cpu12_t c;
	setup(&c, 0x4000);
	ppage = 0x3D;
	const uint8_t prog[] = { 0x4A, 0x90, 0x00, 0x3C };   /* CALL $9000,$3C */
	put(0x4000, prog, 4);
	mem[0x9000] = 0x0A;                       /* RTC */
	cpu12_step(&c);
	CHECK(c.pc == 0x9000 && ppage == 0x3C && c.sp == 0x2000 - 3 && mem[c.sp] == 0x3D, "CALL stacks PPAGE");
	cpu12_step(&c);
	CHECK(c.pc == 0x4004 && ppage == 0x3D && c.sp == 0x2000, "RTC restores PPAGE and PC");
}

static void test_x_bit(void)
{
	cpu12_t c;
	setup(&c, 0x4000);
	const uint8_t prog[] = { 0x10, 0xBF, 0x14, 0x40, 0x86, 0xFF, 0xB7, 0x02 };   /* ANDCC #$BF; ORCC #$40; LDAA #$FF; TAP */
	put(0x4000, prog, sizeof prog);
	cpu12_step(&c);
	CHECK(!(c.ccr & CCR_X), "ANDCC clears X");
	cpu12_step(&c);
	CHECK(!(c.ccr & CCR_X), "ORCC cannot set X");
	cpu12_step(&c);
	cpu12_step(&c);
	CHECK(c.ccr == 0xBF, "TAP sets everything but X, CCR=%02X", c.ccr);
}

int main(void)
{
	test_interrupt_frame();
	test_rti_chain();
	test_cli_delay();
	test_wai();
	test_swi_trap();
	test_call_rtc();
	test_x_bit();
	printf("%s (%d failures)\n", failures ? "FAILED" : "all CPU tests passed", failures);
	return failures != 0;
}
