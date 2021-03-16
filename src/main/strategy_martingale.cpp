#include "strategy_martingale.h"

#include <imtjson/object.h>
#include "sgn.h"

std::string_view Strategy_Martingale::id = "martingale";

Strategy_Martingale::Strategy_Martingale(const Config &cfg) :
		cfg(cfg) {
}

Strategy_Martingale::Strategy_Martingale(const Config &cfg, State &&st)
	:cfg(cfg),st(std::move(st))
{

}

bool Strategy_Martingale::isValid() const {
	return st.price > 0 && st.enter_price > 0 && st.exit_price > 0 && st.budget > 0;
}

PStrategy Strategy_Martingale::importState(json::Value src, const IStockApi::MarketInfo &minfo) const {
	return new Strategy_Martingale(cfg,{
			src["pos"].getNumber(),
			src["price"].getNumber(),
			src["enter_price"].getNumber(),
			src["exit_price"].getNumber(),
			src["value"].getNumber(),
			src["budget"].getNumber(),
	});
}

json::Value Strategy_Martingale::exportState() const {
	return json::Object
			("pos", st.pos)
			("price", st.price)
			("enter_price", st.enter_price)
			("exit_price", st.exit_price)
			("value", st.value)
			("budget", st.budget);
}

PStrategy Strategy_Martingale::init(double pos, double price, double currency) const {

	if (cfg.initial_step<=0) throw std::runtime_error("Configuration error: initial step is zero");
	if (cfg.power<=0) throw std::runtime_error("Configuration error: power is zero");
	if (cfg.reduction <0) throw std::runtime_error("Configuration error: reduction is zero");
	State nwst;
	double liq_price = pos?(-currency/pos + price):price;
	nwst.exit_price = std::min(std::max(price*0.5, 2*price - liq_price),price*2.0);
	nwst.enter_price = nwst.exit_price;
	nwst.pos = pos;
	nwst.price = price;
	nwst.value = (nwst.exit_price - price) * pos*0.5;;
	nwst.budget = cfg.collateral?cfg.collateral:currency+nwst.value;
	PStrategy s = new Strategy_Martingale(cfg, std::move(nwst));
	if (s->isValid()) return s;
	else throw std::runtime_error("Unable to initialze strategy");

}

IStrategy::OrderData Strategy_Martingale::getNewOrder(
		const IStockApi::MarketInfo &minfo, double cur_price, double new_price,
		double dir, double assets, double currency, bool rej) const {

	double pos = calcPos(new_price);
	double sz = pos - assets;
	return OrderData{0,sz};

}

PStrategy Strategy_Martingale::onIdle(const IStockApi::MarketInfo &minfo,
		const IStockApi::Ticker &curTicker, double assets,
		double currency) const {
	if (isValid()) return PStrategy(this);
	else return init(assets, curTicker.last, currency);
}

PStrategy Strategy_Martingale::reset() const {
	return new Strategy_Martingale(cfg);
}

std::pair<IStrategy::OnTradeResult, PStrategy > Strategy_Martingale::onTrade(
		const IStockApi::MarketInfo &minfo, double tradePrice, double tradeSize,
		double assetsLeft, double currencyLeft) const {

	if (!isValid()) return init(assetsLeft, tradePrice, currencyLeft)->onTrade(minfo, tradePrice, tradeSize, assetsLeft, currencyLeft);

	double profit = (tradePrice - st.price) * (assetsLeft-tradeSize);
	double internal_pos = calcPos(tradePrice);
	double internal_sz = internal_pos - st.pos;
	double xp = internal_sz * st.pos;
	State nwst;
	if (xp == 0) {
		nwst.enter_price = nwst.exit_price = tradePrice;
	} else if (xp > 0) {
		double red = (200.0/(100*cfg.reduction+1));
		nwst.enter_price = (st.enter_price * st.pos + tradePrice * internal_sz) / internal_pos;
		nwst.exit_price = (st.exit_price * red +st.enter_price)/(red+1.0);
	} else {
		nwst.enter_price = st.enter_price;
		nwst.exit_price = st.exit_price;
	}
	nwst.price = tradePrice;
	nwst.pos = internal_pos;
	nwst.value = (nwst.exit_price - tradePrice) * internal_pos*0.5;
	nwst.budget = cfg.collateral?cfg.collateral:currencyLeft+nwst.value;
	double vchange = (profit - ( st.value - nwst.value));

	return {
		{vchange, 0, nwst.exit_price, nwst.enter_price},
		new Strategy_Martingale(cfg, std::move(nwst))
	};




}

IStrategy::MinMax Strategy_Martingale::calcSafeRange(const IStockApi::MarketInfo &minfo, double assets, double currencies) const {
	if (st.pos) return {0, std::numeric_limits<double>::infinity()};
	double liq_price = (-currencies/assets + st.price);
	if (liq_price < st.price) return {liq_price, std::numeric_limits<double>::infinity()};
	else return {0, liq_price};
}

std::string_view Strategy_Martingale::getID() const {
	return id;
}

double Strategy_Martingale::calcInitialPosition(const IStockApi::MarketInfo &, double,  double ,double ) const {
	return 0;
}

IStrategy::BudgetInfo Strategy_Martingale::getBudgetInfo() const {
	return {st.budget,0};
}

double Strategy_Martingale::getEquilibrium(double assets) const {
	double posFrac = assets/st.pos;
	return st.exit_price+posFrac * (st.price - st.exit_price);
}

double Strategy_Martingale::calcCurrencyAllocation(double ) const {
	return st.budget;
}

IStrategy::ChartPoint Strategy_Martingale::calcChart(double price) const {
	return {false};
}

json::Value Strategy_Martingale::dumpStatePretty( const IStockApi::MarketInfo &minfo) const {

	return json::Object
			("Position", st.pos)
			("Enter price", minfo.invert_price?1.0/st.enter_price:st.enter_price)
			("Exit price", minfo.invert_price?1.0/st.exit_price:st.exit_price)
			("Value", st.value)
			("Budget", st.budget);
}

bool Strategy_Martingale::needLiveBalance() const {
	return false;
}

double Strategy_Martingale::calcPos(double new_price) const {
	double dir = sgn(new_price - st.price);
	double init_vol = st.budget * cfg.initial_step/new_price;
	if (dir * st.pos > 0) {

		double f = (new_price - st.price)/(st.exit_price - st.price);
		if (std::isfinite(f) && f >= 0 && f < 1.0) {
			double new_pos = (1.0 - pow2(f))*st.pos;
			if (std::abs(new_pos) < init_vol) new_pos = 0.0;
			return new_pos;
		} else {
			//f is not finite - probably exit_price = price, so exit as soon as possible
			//f is above 1.0, which means that we are beyond exit price, so exit now
			//f is below 0.0, which means exit price is on other side than new price, so we are still beyond exit price, so exit now
			return 0.0; //unexpected situation, close position now
		}
	} else if (st.pos == 0) {
		return -dir * init_vol;
	} else {
		return st.pos * (cfg.power+1.0);
	}
}