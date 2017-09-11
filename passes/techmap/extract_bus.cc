/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2017  Clifford Wolf <clifford@clifford.at>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "kernel/modtools.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

/*
//get the list of cells hooked up to at least one bit of a given net
pool<Cell*> get_other_cells(const RTLIL::SigSpec& port, ModIndex& index, Cell* src)
{
	pool<Cell*> rval;
	for(auto b : port)
	{
		pool<ModIndex::PortInfo> ports = index.query_ports(b);
		for(auto x : ports)
		{
			if(x.cell == src)
				continue;
			rval.insert(x.cell);
		}
	}
	return rval;
}

//return true if there is a full-width bus connection from cell a port ap to cell b port bp
//if other_conns_allowed is false, then we require a strict point to point connection (no other links)
bool is_full_bus(
	const RTLIL::SigSpec& sig,
	ModIndex& index,
	Cell* a,
	RTLIL::IdString ap,
	Cell* b,
	RTLIL::IdString bp,
	bool other_conns_allowed = false)
{
	for(auto s : sig)
	{
		pool<ModIndex::PortInfo> ports = index.query_ports(s);
		bool found_a = false;
		bool found_b = false;
		for(auto x : ports)
		{
			if( (x.cell == a) && (x.port == ap) )
				found_a = true;
			else if( (x.cell == b) && (x.port == bp) )
				found_b = true;
			else if(!other_conns_allowed)
				return false;
		}

		if( (!found_a) || (!found_b) )
			return false;
	}

	return true;
}

//return true if the signal connects to one port only (nothing on the other end)
bool is_unconnected(const RTLIL::SigSpec& port, ModIndex& index)
{
	for(auto b : port)
	{
		pool<ModIndex::PortInfo> ports = index.query_ports(b);
		if(ports.size() > 1)
			return false;
	}

	return true;
}

struct CounterExtraction
{
	int width;						//counter width
	RTLIL::Wire* rwire;				//the register output
	bool has_reset;					//true if we have a reset
	RTLIL::SigSpec rst;				//reset pin
	int count_value;				//value we count from
	RTLIL::SigSpec clk;				//clock signal
	RTLIL::SigSpec outsig;			//counter output signal
	RTLIL::Cell* count_mux;			//counter mux
	RTLIL::Cell* count_reg;			//counter register
	RTLIL::Cell* underflow_inv;		//inverter reduction for output-underflow detect
	pool<ModIndex::PortInfo> pouts;	//Ports that take a parallel output from us
};
*/
/*
//attempt to extract a counter centered on the given adder cell
//For now we only support DOWN counters.
//TODO: up/down support
int bus_tryextract(
	ModIndex& index,
	Cell *cell,
	CounterExtraction& extract,
	pool<RTLIL::IdString>& parallel_cells,
	int maxwidth)
{
	SigMap& sigmap = index.sigmap;

	//A counter with less than 2 bits makes no sense
	//TODO: configurable min threshold
	int a_width = cell->getParam("\\A_WIDTH").as_int();
	extract.width = a_width;
	if( (a_width < 2) || (a_width > maxwidth) )
		return 1;

	//Second input must be a single bit
	int b_width = cell->getParam("\\B_WIDTH").as_int();
	if(b_width != 1)
		return 2;

	//Both inputs must be unsigned, so don't extract anything with a signed input
	bool a_sign = cell->getParam("\\A_SIGNED").as_bool();
	bool b_sign = cell->getParam("\\B_SIGNED").as_bool();
	if(a_sign || b_sign)
		return 3;

	//To be a counter, one input of the ALU must be a constant 1
	//TODO: can A or B be swapped in synthesized RTL or is B always the 1?
	const RTLIL::SigSpec b_port = sigmap(cell->getPort("\\B"));
	if(!b_port.is_fully_const() || (b_port.as_int() != 1) )
		return 4;

	//BI and CI must be constant 1 as well
	const RTLIL::SigSpec bi_port = sigmap(cell->getPort("\\BI"));
	if(!bi_port.is_fully_const() || (bi_port.as_int() != 1) )
		return 5;
	const RTLIL::SigSpec ci_port = sigmap(cell->getPort("\\CI"));
	if(!ci_port.is_fully_const() || (ci_port.as_int() != 1) )
		return 6;

	//CO and X must be unconnected (exactly one connection to each port)
	if(!is_unconnected(sigmap(cell->getPort("\\CO")), index))
		return 7;
	if(!is_unconnected(sigmap(cell->getPort("\\X")), index))
		return 8;

	//Y must have exactly one connection, and it has to be a $mux cell.
	//We must have a direct bus connection from our Y to their A.
	const RTLIL::SigSpec aluy = sigmap(cell->getPort("\\Y"));
	pool<Cell*> y_loads = get_other_cells(aluy, index, cell);
	if(y_loads.size() != 1)
		return 9;
	Cell* count_mux = *y_loads.begin();
	extract.count_mux = count_mux;
	if(count_mux->type != "$mux")
		return 10;
	if(!is_full_bus(aluy, index, cell, "\\Y", count_mux, "\\A"))
		return 11;

	//B connection of the mux is our underflow value
	const RTLIL::SigSpec underflow = sigmap(count_mux->getPort("\\B"));
	if(!underflow.is_fully_const())
		return 12;
	extract.count_value = underflow.as_int();

	//S connection of the mux must come from an inverter (need not be the only load)
	const RTLIL::SigSpec muxsel = sigmap(count_mux->getPort("\\S"));
	extract.outsig = muxsel;
	pool<Cell*> muxsel_conns = get_other_cells(muxsel, index, count_mux);
	Cell* underflow_inv = NULL;
	for(auto c : muxsel_conns)
	{
		if(c->type != "$logic_not")
			continue;
		if(!is_full_bus(muxsel, index, c, "\\Y", count_mux, "\\S", true))
			continue;

		underflow_inv = c;
		break;
	}
	if(underflow_inv == NULL)
		return 13;
	extract.underflow_inv = underflow_inv;

	//Y connection of the mux must have exactly one load, the counter's internal register
	const RTLIL::SigSpec muxy = sigmap(count_mux->getPort("\\Y"));
	pool<Cell*> muxy_loads = get_other_cells(muxy, index, count_mux);
	if(muxy_loads.size() != 1)
		return 14;
	Cell* count_reg = *muxy_loads.begin();
	extract.count_reg = count_reg;
	if(count_reg->type == "$dff")
		extract.has_reset = false;
	else if(count_reg->type == "$adff")
	{
		extract.has_reset = true;

		//Verify ARST_VALUE is zero and ARST_POLARITY is 1
		//TODO: infer an inverter to make it 1 if necessary, so we can support negative level resets?
		if(count_reg->getParam("\\ARST_POLARITY").as_int() != 1)
			return 22;
		if(count_reg->getParam("\\ARST_VALUE").as_int() != 0)
			return 23;

		//Save the reset
		extract.rst = sigmap(count_reg->getPort("\\ARST"));
	}
	//TODO: support synchronous reset
	else
		return 15;
	if(!is_full_bus(muxy, index, count_mux, "\\Y", count_reg, "\\D"))
		return 16;

	//TODO: Verify count_reg CLK_POLARITY is 1

	//Register output must have exactly two loads, the inverter and ALU
	//(unless we have a parallel output!)
	const RTLIL::SigSpec qport = count_reg->getPort("\\Q");
	const RTLIL::SigSpec cnout = sigmap(qport);
	pool<Cell*> cnout_loads = get_other_cells(cnout, index, count_reg);
	if(cnout_loads.size() > 2)
	{
		//If we specified a limited set of cells for parallel output, check that we only drive them
		if(!parallel_cells.empty())
		{
			for(auto c : cnout_loads)
			{
				if(c == underflow_inv)
					continue;
				if(c == cell)
					continue;

				//Make sure we're in the whitelist
				if( parallel_cells.find(c->type) == parallel_cells.end())
					return 17;

				//Figure out what port(s) are driven by it
				//TODO: this can probably be done more efficiently w/o multiple iterations over our whole net?
				RTLIL::IdString portname;
				for(auto b : qport)
				{
					pool<ModIndex::PortInfo> ports = index.query_ports(b);
					for(auto x : ports)
					{
						if(x.cell != c)
							continue;
						if(portname == "")
							portname = x.port;

						//somehow our counter output is going to multiple ports
						//this makes no sense, don't allow inference
						else if(portname != x.port)
							return 17;
					}
				}

				//Save the other loads
				extract.pouts.insert(ModIndex::PortInfo(c, portname, 0));
			}
		}
	}
	if(!is_full_bus(cnout, index, count_reg, "\\Q", underflow_inv, "\\A", true))
		return 18;
	if(!is_full_bus(cnout, index, count_reg, "\\Q", cell, "\\A", true))
		return 19;

	//Look up the clock from the register
	extract.clk = sigmap(count_reg->getPort("\\CLK"));

	//Register output net must have an INIT attribute equal to the count value
	extract.rwire = cnout.as_wire();
	if(extract.rwire->attributes.find("\\init") == extract.rwire->attributes.end())
		return 20;
	int rinit = extract.rwire->attributes["\\init"].as_int();
	if(rinit != extract.count_value)
		return 21;

	return 0;
}
*/

void bus_worker(
	ModIndex& index,
	Cell *cell,
	unsigned int& total_buses)
{
	SigMap& sigmap = index.sigmap;

	/*
		Table of ports on each cell that are guaranteed to be a bus.

		Note that with commutative operations such as addition, we cannot make the inputs buses
		because we don't know which bits belong to which input.
		We also cannot infer a bus from
	 */
	//TODO: better way to store this table
	std::set<RTLIL::IdString> busports;
	if(cell->type == "$add")
		busports.emplace("\\Y");
	else if(cell->type == "$__COUNT_")
		busports.emplace("\\POUT");

	//Unknown cell, bus ports not supported
	else
		return;

	//Bus-ify each port
	log("Inferring buses for port(s) of %s\n", log_id(cell->name));
	for(auto p : busports)
	{
		//See what's on the port now
		//If it's a wire, stop - no action needed
		//If there's nothing on the port, skip it as well!
		if(!cell->hasPort(p))
		{
			//log("  Cell does not have a port named %s\n", p.c_str());
			continue;
		}
		auto port = sigmap(cell->getPort(p));
		if(port.is_wire())
		{
			//log("  Port %s is already a wire\n", p.c_str());
			continue;
		}

		//Not a wire, create one!
		auto wire = cell->module->addWire(NEW_ID, port.size());

		//Find downstream entities connected to the old wire and reconnect them
		log("  Inferring bus for port %s\n", p.c_str());
		for(int i=0; i<port.size(); i++)
		{
			auto b = port[i];
			pool<ModIndex::PortInfo> ports = index.query_ports(b);

			log("    Finding additional loads for bit %d / signal %s (found %zu)\n",
				i, log_id(b.wire->name), ports.size());
			for(auto x : ports)
			{
				if(x.cell == cell)
					continue;
				log("      Cell %s port %s is connected to us at offset %d\n",
					log_id(x.cell->name), log_id(x.port), x.offset);

				//Patch the sigspec one bit at a time as needed
				auto dspec = x.cell->getPort(x.port);
				dspec[x.offset] = RTLIL::SigBit(wire, i);
				x.cell->setPort(x.port, dspec);
			}

			//See if this signal is a top-level module port
			//If so, add buffers to preserve the existing signal names
			//TODO: how to handle top level vector ports?
			bool found = false;
			for(auto p : cell->module->ports)
			{
				if(p == b.wire->name)
					found = true;
			}

			if(found)
			{
				log("      Cell %s port %s drives top level module port %s\n",
					log_id(cell->name), p.c_str(), log_id(b.wire->name));

				cell->module->addBufGate(NEW_ID, RTLIL::SigSpec(wire, i, 1), b.wire);
			}
		}

		//Hook up the new wire
		cell->setPort(p, wire);

		//We made a match, note it
		total_buses ++;
	}
}

struct ExtractBusPass : public Pass {
	ExtractBusPass() : Pass("extract_bus", "Extract buses from split nets") { }
	virtual void help()
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    extract_bus [selection]\n");
		log("\n");
		log("This pass finds buses in a design consisting of single-bit nets\n");
		log("\n");
	}
	virtual void execute(std::vector<std::string> args, RTLIL::Design *design)
	{
		log_header(design, "Executing EXTRACT_BUS pass (find buses in netlist).\n");

		//int argidx = 0;
		//extra_args(args, argidx, design);

		//Extract all of the buses we could find
		unsigned int total_buses = 0;
		for (auto module : design->selected_modules())
		{
			ModIndex index(module);
			for (auto cell : module->selected_cells())
				bus_worker(index, cell, total_buses);
		}

		//TODO: Recurse and find additional buses by elimination

		if(total_buses)
			log("Extracted %u buses\n", total_buses);
	}
} ExtractBusPass;

PRIVATE_NAMESPACE_END
