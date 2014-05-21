#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile_impl.h"
#include "chrome/browser/browser_process_impl.h"
#include <string>
#include <vector>

class PrincipalManager{
public:
	PrincipalManager();
	~PrincipalManager();
	//for navigation (derivative content allocation)
	ProfileImpl* getTargetPrincipal(ProfileImpl* source, std::string domain);
	//only for address bar navigation (initial content allocation)
	ProfileImpl* getStartingPointPrincipal(std::string domain);

	/*
	 * For both regular principal and starting point principal
	 *   depending on whetehr parent is NULL
	 * This method also needs to update 
	 */
	void buildPrincipal(std::string domain, ProfileImpl* parent);

private:
	void initAndRunPrincipal(rofile* p, Profile::CreateStatus status);
	ProfileImpl* getAncestorPrincipal(ProfileImpl* cur, std::string domain);
	std::vector<ProfileImpl*> starting_principals;
};