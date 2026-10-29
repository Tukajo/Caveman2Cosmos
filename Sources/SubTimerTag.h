#pragma once

#ifndef SUB_TIMER_TAG_H
#define SUB_TIMER_TAG_H

#ifdef _WIN32
#include <windows.h>
#else
#include <ctime>
struct LARGE_INTEGER
{
	long long QuadPart;
};
inline int QueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	lpPerformanceCount->QuadPart = (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
	return 1;
}
inline int QueryPerformanceFrequency(LARGE_INTEGER* lpFrequency)
{
	lpFrequency->QuadPart = 1000000000LL;
	return 1;
}
#endif

#ifndef MAX_PLAYERS
#include "CvDefines.h"
#endif

void addPlayerPatrolMs(int iPlayer, double dMs);
void addPlayerAnyAttackMs(int iPlayer, double dMs);
void addPlayerExposedDangerMs(int iPlayer, double dMs);
void addPlayerAttackOddsMs(int iPlayer, double dMs);
void addPlayerMergeCheckMs(int iPlayer, double dMs);
void addPlayerReachableMs(int iPlayer, double dMs);
void addPlayerPathMs(int iPlayer, double dMs);
void addPlayerDangerQueryMs(int iPlayer, double dMs, bool bInsidePath);
void addPlayerUnitMoveMs(int iPlayer, double dMs);
void addPlayerGroupUpdateMs(int iPlayer, double dMs);

extern int g_iPatrolDepth;
extern int g_iAnyAttackDepth;
extern int g_iExposedDangerDepth;
extern int g_iAttackOddsDepth;
extern int g_iMergeCheckDepth;
extern int g_iReachableDepth;
extern int g_iPathTimingDepth;
extern int g_iDangerQueryTimingDepth;

// Non-recursive RAII timer scope (for functions with multiple return paths like
// CvUnitAI::doUnitAIMove and CvSelectionGroupAI::AI_update).
struct SimpleTimerScope
{
	typedef void (*AddFunc)(int, double);

	SimpleTimerScope(int iPlayer, AddFunc pAddFunc)
		: m_iPlayer(iPlayer), m_pAddFunc(pAddFunc), m_bActive(false)
	{
		if (m_iPlayer >= 0 && m_iPlayer < MAX_PLAYERS && m_pAddFunc != NULL)
		{
			QueryPerformanceCounter(&m_tStart);
			m_bActive = true;
		}
	}

	~SimpleTimerScope()
	{
		if (m_bActive)
		{
			LARGE_INTEGER freq, tEnd;
			QueryPerformanceFrequency(&freq);
			QueryPerformanceCounter(&tEnd);
			m_pAddFunc(m_iPlayer, (double)(tEnd.QuadPart - m_tStart.QuadPart) * 1000.0 / (double)freq.QuadPart);
		}
	}

private:
	int m_iPlayer;
	AddFunc m_pAddFunc;
	LARGE_INTEGER m_tStart;
	bool m_bActive;
};

// Recursion-safe RAII timer scope (measures outermost invocation only).
struct SubTimerScope
{
	typedef void (*AddFunc)(int, double);

	SubTimerScope(int iPlayer, AddFunc pAddFunc, int& rDepth)
		: m_iPlayer(iPlayer), m_pAddFunc(pAddFunc), m_prDepth(NULL), m_bActive(false)
	{
		if (m_iPlayer >= 0 && m_iPlayer < MAX_PLAYERS && m_pAddFunc != NULL)
		{
			m_prDepth = &rDepth;
			if ((*m_prDepth)++ == 0)
			{
				QueryPerformanceCounter(&m_tStart);
				m_bActive = true;
			}
		}
	}

	~SubTimerScope()
	{
		if (m_bActive)
		{
			LARGE_INTEGER freq, tEnd;
			QueryPerformanceFrequency(&freq);
			QueryPerformanceCounter(&tEnd);
			m_pAddFunc(m_iPlayer, (double)(tEnd.QuadPart - m_tStart.QuadPart) * 1000.0 / (double)freq.QuadPart);
		}
		if (m_prDepth)
		{
			--(*m_prDepth);
		}
	}

private:
	int m_iPlayer;
	AddFunc m_pAddFunc;
	int* m_prDepth;
	LARGE_INTEGER m_tStart;
	bool m_bActive;
};

struct PathTimingScope : public SubTimerScope
{
	explicit PathTimingScope(int iPlayer)
		: SubTimerScope(iPlayer, addPlayerPathMs, g_iPathTimingDepth)
	{
	}
};

struct DangerQueryTimingScope
{
	explicit DangerQueryTimingScope(int iPlayer)
		: m_iPlayer(iPlayer), m_bOuter(false)
	{
		if (m_iPlayer >= 0 && m_iPlayer < MAX_PLAYERS)
		{
			if (g_iDangerQueryTimingDepth++ == 0)
			{
				QueryPerformanceCounter(&m_tStart);
				m_bOuter = true;
			}
		}
	}

	~DangerQueryTimingScope()
	{
		if (m_iPlayer >= 0 && m_iPlayer < MAX_PLAYERS)
		{
			--g_iDangerQueryTimingDepth;
			if (m_bOuter)
			{
				LARGE_INTEGER tEnd, freq;
				QueryPerformanceCounter(&tEnd);
				QueryPerformanceFrequency(&freq);
				addPlayerDangerQueryMs(m_iPlayer, (double)(tEnd.QuadPart - m_tStart.QuadPart) * 1000.0 / (double)freq.QuadPart, g_iPathTimingDepth > 0);
			}
		}
	}

private:
	int m_iPlayer;
	bool m_bOuter;
	LARGE_INTEGER m_tStart;
};

#define SUB_TIMER_CONCAT_DETAIL(x, y) x##y
#define SUB_TIMER_CONCAT(x, y) SUB_TIMER_CONCAT_DETAIL(x, y)

#define SUB_TIMER_PATROL(iPlayer) SubTimerScope SUB_TIMER_CONCAT(timer_patrol_, __LINE__)(iPlayer, addPlayerPatrolMs, g_iPatrolDepth)
#define SUB_TIMER_ANYATTACK(iPlayer) SubTimerScope SUB_TIMER_CONCAT(timer_anyattack_, __LINE__)(iPlayer, addPlayerAnyAttackMs, g_iAnyAttackDepth)
#define SUB_TIMER_EXPOSEDDANGER(iPlayer) SubTimerScope SUB_TIMER_CONCAT(timer_danger_, __LINE__)(iPlayer, addPlayerExposedDangerMs, g_iExposedDangerDepth)
#define SUB_TIMER_ATTACKODDS(iPlayer) SubTimerScope SUB_TIMER_CONCAT(timer_odds_, __LINE__)(iPlayer, addPlayerAttackOddsMs, g_iAttackOddsDepth)
#define SUB_TIMER_MERGECHECK(iPlayer) SubTimerScope SUB_TIMER_CONCAT(timer_merge_, __LINE__)(iPlayer, addPlayerMergeCheckMs, g_iMergeCheckDepth)
#define SUB_TIMER_REACHABLE(iPlayer) SubTimerScope SUB_TIMER_CONCAT(timer_reach_, __LINE__)(iPlayer, addPlayerReachableMs, g_iReachableDepth)

#endif // SUB_TIMER_TAG_H
